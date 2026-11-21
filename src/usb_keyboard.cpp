#include "usb_keyboard.hpp"

#include <algorithm>
#include <atomic>
#include <string_view>

#ifdef STACKTERMINAL_USB_DIAGNOSTICS
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "soc/usb_dwc_struct.h"
#endif

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid_host.h"

namespace usbKeyboard {
namespace {

using Handle = hid_host_device_handle_t;
constexpr auto tag = "usb-keyboard";
std::atomic<Handle> active{};
std::atomic<uint8_t> lockLeds{};
QueueHandle_t reports;
QueueHandle_t devices;

#ifdef STACKTERMINAL_USB_DIAGNOSTICS
QueueHandle_t diagnostics;
std::atomic<vprintf_like_t> consolePrint{std::vprintf};
std::atomic<bool> firstReport;

int captureUsbLog(const char* format, va_list args) {
    va_list copy;
    va_copy(copy, args);
    const int printed = consolePrint.load()(format, args);
    Diagnostic line{};
    std::vsnprintf(line.data(), line.size(), format, copy);
    va_end(copy);

    // ESP-IDF v5.5.3 log format includes "(time) tag: message".
    std::string_view message(line.data());
    const auto start = message.find(") ");
    if (start == std::string_view::npos) return printed;
    message.remove_prefix(start + 2);
    const auto logTag = message.substr(0, message.find(':'));
    constexpr std::string_view usbTags[]{
        "usb-keyboard", "hid-host", "USB HOST", "USBH", "HUB", "EXT_HUB", "EXT_PORT", "ENUM", "HCD DWC"
    };
    if (std::find(std::begin(usbTags), std::end(usbTags), logTag) != std::end(usbTags)) {
        line[std::strcspn(line.data(), "\r\n")] = '\0';
        xQueueSend(diagnostics, &line, 0);
    }
    return printed;
}
#endif

struct DeviceEvent {
    Handle handle;
    bool attach;
};

void pushReport(const Report& report) {
    if (xQueueSend(reports, &report, 0) != pdTRUE) {
        Report old{};
        xQueueReceive(reports, &old, 0);
        xQueueSend(reports, &report, 0);
    }
}

void interfaceEvent(Handle handle, hid_host_interface_event_t event, void*) {
    if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        Handle expected = handle;
        if (active.compare_exchange_strong(expected, nullptr)) {
            pushReport({});
#ifdef STACKTERMINAL_USB_DIAGNOSTICS
            ESP_LOGI(tag, "Keyboard unplugged. Waiting for another attachment.");
#endif
        }
        const DeviceEvent notice{handle, false};
        if (xQueueSend(devices, &notice, 0) != pdTRUE) {
            ESP_LOGE(tag, "USB device event queue full");
        }
    } else if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT && active.load() == handle) {
        constexpr size_t bootReportSize = 8;
#ifdef STACKTERMINAL_USB_DIAGNOSTICS
        uint8_t raw[64]{};
#else
        uint8_t raw[bootReportSize]{};
#endif
        size_t size = 0;
        if (hid_host_device_get_raw_input_report_data(handle, raw, sizeof(raw), &size) != ESP_OK) {
            return;
        }
#ifdef STACKTERMINAL_USB_DIAGNOSTICS
        if (firstReport.exchange(false)) {
            ESP_LOGI(tag, "First HID input report: %u bytes", static_cast<unsigned>(size));
        }
#endif
        if (size >= bootReportSize) {
            Report report{raw[0], {}};
            std::copy_n(raw + 2, report.keys.size(), report.keys.begin());
            pushReport(report);
        }
    } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR && active.load() == handle) {
        pushReport({});
        ESP_LOGW(tag, "USB keyboard transfer error");
    }
}

void driverEvent(Handle handle, hid_host_driver_event_t, void*) {
    hid_host_dev_params_t params{};
    if (hid_host_device_get_params(handle, &params) != ESP_OK) return;
    const bool bootKeyboard = params.sub_class == HID_SUBCLASS_BOOT_INTERFACE
                              && params.proto == HID_PROTOCOL_KEYBOARD;
#ifdef STACKTERMINAL_USB_DIAGNOSTICS
    hid_host_dev_info_t info{};
    hid_host_get_device_info(handle, &info);
    ESP_LOGI(tag, "HID %04X:%04X addr %u iface %u subclass %u protocol %u (%s)",
             info.VID, info.PID, params.addr, params.iface_num, params.sub_class, params.proto,
             bootKeyboard ? "Boot keyboard" : "ignored");
#endif
    if (bootKeyboard) {
        const DeviceEvent notice{handle, true};
        if (xQueueSend(devices, &notice, 0) != pdTRUE) {
            ESP_LOGE(tag, "USB device event queue full");
        }
    }
}

void openKeyboard(Handle handle) {
    // Reserve the handle before installing the callback, so a detach during setup can clear it.
    active.store(handle);
    const hid_host_device_config_t interfaceConfig{interfaceEvent, nullptr};
    esp_err_t result = hid_host_device_open(handle, &interfaceConfig);
    if (result != ESP_OK) {
        Handle expected = handle;
        active.compare_exchange_strong(expected, nullptr);
        ESP_LOGW(tag, "Opening keyboard: %s", esp_err_to_name(result));
        return;
    }
    if (active.load() != handle) return;
    result = hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT);
    if (result == ESP_OK && active.load() != handle) return;
    if (result == ESP_OK) {
#ifdef STACKTERMINAL_USB_DIAGNOSTICS
        firstReport.store(true);
#endif
        result = hid_host_device_start(handle);
        if (result != ESP_OK) ESP_LOGW(tag, "Starting input: %s", esp_err_to_name(result));
    } else {
        ESP_LOGW(tag, "SET_PROTOCOL (Boot): %s", esp_err_to_name(result));
    }
    if (result != ESP_OK) {
        Handle expected = handle;
        if (active.compare_exchange_strong(expected, nullptr)) {
            hid_host_device_close(handle);
        }
#ifdef STACKTERMINAL_USB_DIAGNOSTICS
    } else if (active.load() == handle) {
        ESP_LOGI(tag, "Boot keyboard ready. Press a key.");
#endif
    }
}

void handleDevices(void*) {
    uint8_t sentLeds = 0xff;
    for (;;) {
        DeviceEvent event{};
        if (xQueueReceive(devices, &event, pdMS_TO_TICKS(30)) == pdTRUE) {
            if (!event.attach) {
                hid_host_device_close(event.handle);
            } else if (!active.load()) {
                openKeyboard(event.handle);
                sentLeds = 0xff;
            }
        }
        Handle handle = active.load();
        const uint8_t desired = lockLeds.load();
        if (handle && desired != sentLeds) {
            uint8_t output = desired;
            hid_class_request_set_report(handle, HID_REPORT_TYPE_OUTPUT, 0, &output, 1);
            sentLeds = desired;
        }
    }
}

void handleUsb(void*) {
    const auto powerOn = usb_host_lib_set_root_port_power(true);
    if (powerOn != ESP_OK) {
        ESP_LOGE(tag, "Enabling USB root port: %s", esp_err_to_name(powerOn));
    }
#ifdef STACKTERMINAL_USB_DIAGNOSTICS
    constexpr auto eventTimeout = pdMS_TO_TICKS(250);
    int deviceCount = -1;
    int previousPortState = -1;
#else
    constexpr auto eventTimeout = portMAX_DELAY;
#endif
    for (;;) {
        uint32_t events = 0;
        if (usb_host_lib_handle_events(eventTimeout, &events) == ESP_OK
            && (events & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)) {
            usb_host_device_free_all();
        }
#ifdef STACKTERMINAL_USB_DIAGNOSTICS
        // Read the ESP32-S3 USB controller without touching the port's change flags.
        usb_dwc_hprt_reg_t port{};
        port.val = USB_DWC.hprt_reg.val;
        const int portState = (port.prtpwr << 2) | (port.prtconnsts << 1) | port.prtena;
        if (portState != previousPortState) {
            previousPortState = portState;
            ESP_LOGI(tag, "USB root: logical power=%u attached=%u enabled=%u line=%u",
                     static_cast<unsigned>(port.prtpwr), static_cast<unsigned>(port.prtconnsts),
                     static_cast<unsigned>(port.prtena), static_cast<unsigned>(port.prtlnsts));
        }
        usb_host_lib_info_t info{};
        if (usb_host_lib_info(&info) == ESP_OK && info.num_devices != deviceCount) {
            deviceCount = info.num_devices;
            ESP_LOGI(tag, "Enumerated USB devices (including hubs): %d", deviceCount);
        }
#endif
    }
}

}

bool start() {
#ifdef STACKTERMINAL_USB_DIAGNOSTICS
    diagnostics = xQueueCreate(24, sizeof(Diagnostic));
    if (diagnostics) consolePrint.store(esp_log_set_vprintf(captureUsbLog));
    ESP_LOGI(tag, "USB diagnostics. Waiting for USB enumeration.");
#endif
    reports = xQueueCreate(64, sizeof(Report));
    devices = xQueueCreate(8, sizeof(DeviceEvent));
    if (!reports || !devices) {
        return false;
    }

    usb_host_config_t usbConfig{};
    // Register the HID client before the host starts enumerating an already connected hub.
    usbConfig.root_port_unpowered = true;
    esp_err_t err = usb_host_install(&usbConfig);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "USB Host: %s", esp_err_to_name(err));
        return false;
    }
    const hid_host_driver_config_t hidConfig{true, 5, 4096, tskNO_AFFINITY, driverEvent, nullptr};
    err = hid_host_install(&hidConfig);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "HID Host: %s", esp_err_to_name(err));
        usb_host_uninstall();
        return false;
    }
    TaskHandle_t keyboardTask = nullptr;
    if (xTaskCreate(handleDevices, "usb-keyboard", 4096, nullptr, 5, &keyboardTask) == pdPASS
        && xTaskCreate(handleUsb, "usb-host", 4096, nullptr, 5, nullptr) == pdPASS) {
        return true;
    }
    ESP_LOGE(tag, "Could not start USB host tasks");
    if (keyboardTask) vTaskDelete(keyboardTask);
    hid_host_uninstall();
    usb_host_uninstall();
    return false;
}

bool connected() { return active.load() != nullptr; }

bool read(Report& report) {
    return reports && xQueueReceive(reports, &report, 0) == pdTRUE;
}

#ifdef STACKTERMINAL_USB_DIAGNOSTICS
bool readDiagnostic(Diagnostic& line) {
    return diagnostics && xQueueReceive(diagnostics, &line, 0) == pdTRUE;
}
#endif

void setLockLeds(bool caps, bool num) {
    lockLeds.store((caps ? 2 : 0) | (num ? 1 : 0));
}

char32_t character(uint8_t key, uint8_t modifiers, bool caps, bool num) {
    if (ctrl(modifiers) || (modifiers & 0x8c)) {
        return 0;
    }
    const bool shifted = shift(modifiers);
    if (key >= 0x04 && key <= 0x1d) {
        const char32_t letter = U'a' + key - 0x04;
        const bool upper = shifted != caps;
        if (!altGr(modifiers)) {
            return upper ? letter - U'a' + U'A' : letter;
        }
        constexpr std::u32string_view base = U"acelnosxz";
        const size_t i = base.find(letter);
        if (i != std::u32string_view::npos) {
            return (upper ? U"ĄĆĘŁŃÓŚŹŻ" : U"ąćęłńóśźż")[i];
        }
        return 0;
    }
    if (altGr(modifiers)) {
        return 0;
    }
    if (key >= 0x1e && key <= 0x27) {
        return (shifted ? U"!@#$%^&*()" : U"1234567890")[key - 0x1e];
    }
    if (key >= space && key <= 0x38) {
        return (shifted ? U" _+{}|~:\"~<>?" : U" -=[]\\#;'`,./")[key - space];
    }
    if (key >= 0x54 && key <= 0x57) {
        return U"/*-+"[key - 0x54];
    }
    if (num && key >= 0x59 && key <= 0x63) {
        return U"1234567890."[key - 0x59];
    }
    return key == 0x64 ? (shifted ? U'|' : U'\\') : 0;
}

}
