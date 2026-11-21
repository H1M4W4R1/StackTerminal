#include <algorithm>
#include <cstdio>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "terminal_view.hpp"
#include "usb_keyboard.hpp"

namespace {

constexpr auto tag = "stackterminal";
constexpr auto port = UART_NUM_1;
constexpr int txPin = 2;
constexpr int rxPin = 1;
constexpr int rxBuffer = 16384;
constexpr int txBuffer = 8192;

Settings settings;
Terminal terminal;
TerminalView view;
nvs_handle_t storage;
QueueHandle_t uartEvents;
bool storageReady = false;
bool uartReady = false;
bool caps = false;
bool num = true;
bool historyDirty = true;
bool inputDirty = true;
int pages = 0;
uint32_t blinkSince = 0;
const char* uartError = nullptr;

void loadSettings() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) {
        err = nvs_open("terminal", NVS_READWRITE, &storage);
    }
    if (err != ESP_OK) {
        ESP_LOGE(tag, "NVS: %s", esp_err_to_name(err));
        return;
    }
    storageReady = true;
    Settings stored;
    size_t length = sizeof(stored);
    err = nvs_get_blob(storage, "settings", &stored, &length);
    if (err == ESP_OK && length == sizeof(stored) && stored.valid()) {
        settings = stored;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(tag, "Invalid or unreadable settings (%s), using defaults", esp_err_to_name(err));
    }
}

esp_err_t configureUart(const Settings& current) {
    constexpr uart_parity_t parity[]{UART_PARITY_DISABLE, UART_PARITY_EVEN, UART_PARITY_ODD};
    constexpr uart_stop_bits_t stop[]{UART_STOP_BITS_1, UART_STOP_BITS_1_5, UART_STOP_BITS_2};
    uart_config_t config{};
    config.baud_rate = static_cast<int>(current.baud);
    config.data_bits = static_cast<uart_word_length_t>(current.bits - 5);
    config.parity = parity[current.parity];
    config.stop_bits = stop[current.stop];
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    return uart_param_config(port, &config);
}

bool startUart() {
    esp_err_t err = configureUart(settings);
    if (err == ESP_OK) err = uart_set_pin(port, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err == ESP_OK) err = uart_driver_install(port, rxBuffer, txBuffer, 48, &uartEvents, 0);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "Grove UART: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void saveSettings() {
    Settings updated;
    if (!view.readSettings(updated)) {
        view.settingsError("Podaj prędkość 300-3000000 i dwa kolory po 6 cyfr HEX.");
        return;
    }
    if (!storageReady) {
        view.settingsError("Pamięć NVS jest niedostępna.");
        return;
    }
    const bool serialChanged = updated.baud != settings.baud || updated.bits != settings.bits
                               || updated.parity != settings.parity || updated.stop != settings.stop;
    if (uartReady && serialChanged && uart_wait_tx_done(port, 0) != ESP_OK) {
        view.settingsError("UART nadal nadaje. Poczekaj na zakończenie transmisji.");
        return;
    }
    if (uartReady && serialChanged && configureUart(updated) != ESP_OK) {
        configureUart(settings);
        view.settingsError("UART odrzucił wybrane parametry.");
        return;
    }
    const esp_err_t saved = nvs_set_blob(storage, "settings", &updated, sizeof(updated));
    if (saved != ESP_OK || nvs_commit(storage) != ESP_OK) {
        if (uartReady && serialChanged) configureUart(settings);
        view.settingsError("Zapis ustawień w NVS nie powiódł się.");
        return;
    }
    settings = updated;
    view.closeSettings();
    historyDirty = inputDirty = true;
}

void transmit(bool suffix) {
    auto bytes = terminal.transmission(suffix);
    if (!uartReady || bytes.empty()) return;

    size_t available = 0;
    const esp_err_t result = uart_get_tx_buffer_free_size(port, &available);
    if (result != ESP_OK || available < bytes.size() + 64) {
        uartError = "Bufor TX pełny. Ponów Enter.";
        return;
    }
    if (uart_write_bytes(port, bytes.data(), bytes.size()) != static_cast<int>(bytes.size())) {
        uartError = "Błąd wysyłania UART.";
        return;
    }
    uartError = nullptr;
    terminal.submitted();
}

void receive() {
    if (!uartReady) return;
    uart_event_t event{};
    bool lostData = false;
    bool invalidFrame = false;
    while (xQueueReceive(uartEvents, &event, 0) == pdTRUE) {
        lostData |= event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL;
        invalidFrame |= event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR;
    }
    if (lostData || invalidFrame) {
        terminal.receiveError(lostData ? U"[UART: utrata danych RX]" : U"[UART: błąd ramki lub parzystości]");
        historyDirty = true;
    }
    char bytes[1024];
    for (int i = 0; i < 8; ++i) {
        const int count = uart_read_bytes(port, bytes, sizeof(bytes), 0);
        if (count <= 0) break;
        historyDirty |= terminal.receive({bytes, static_cast<size_t>(count)});
    }
}

void keyboardAction(uint8_t key, uint8_t modifiers) {
    const bool ctrl = usbKeyboard::ctrl(modifiers);
    const bool shift = usbKeyboard::shift(modifiers);
    if (key == usbKeyboard::tab && ctrl) {
        if (view.settingsOpen()) {
            view.closeSettings();
        } else {
            view.openSettings(settings);
        }
        inputDirty = true;
        return;
    }
    if (key == usbKeyboard::capsLock || key == usbKeyboard::numLock) {
        if (key == usbKeyboard::capsLock) caps = !caps;
        else num = !num;
        usbKeyboard::setLockLeds(caps, num);
        return;
    }
    if (!num) {
        switch (key) {
            case 0x59: key = usbKeyboard::end; break;
            case 0x5a: key = usbKeyboard::down; break;
            case 0x5b: key = usbKeyboard::pageDown; break;
            case 0x5c: key = usbKeyboard::left; break;
            case 0x5e: key = usbKeyboard::right; break;
            case 0x5f: key = usbKeyboard::home; break;
            case 0x60: key = usbKeyboard::up; break;
            case 0x61: key = usbKeyboard::pageUp; break;
            case 0x62: key = usbKeyboard::insert; break;
            case 0x63: key = usbKeyboard::del; break;
        }
    }
    const bool enter = key == usbKeyboard::enter || key == usbKeyboard::keypadEnter;
    const char32_t c = usbKeyboard::character(key, modifiers, caps, num);
    if (view.settingsOpen()) {
        if (enter) saveSettings();
        else if (key == usbKeyboard::escape) view.closeSettings();
        else if (key == usbKeyboard::up || key == usbKeyboard::down || key == usbKeyboard::tab)
            view.selectSetting((key == usbKeyboard::up || (key == usbKeyboard::tab && shift)) ? -1 : 1);
        else if (key == usbKeyboard::left || key == usbKeyboard::right)
            view.adjustSetting(key == usbKeyboard::left ? -1 : 1);
        else if (key == usbKeyboard::backspace || c)
            view.editSetting(key == usbKeyboard::backspace ? U'\b' : c);
        return;
    }
    if (enter) {
        if (ctrl) transmit(false);
        else if (shift) terminal.insertLineBreak();
        else transmit(true);
    } else {
        switch (key) {
            case usbKeyboard::up:
                if (ctrl) settings.font = std::min(settings.font + 1, uint32_t{4});
                else terminal.recall(true);
                break;
            case usbKeyboard::down:
                if (ctrl && settings.font > 0) --settings.font;
                if (!ctrl) terminal.recall(false);
                break;
            case usbKeyboard::pageUp: --pages; break;
            case usbKeyboard::pageDown: ++pages; break;
            case usbKeyboard::left: terminal.moveLeft(); break;
            case usbKeyboard::right: terminal.moveRight(); break;
            case usbKeyboard::home: terminal.moveHome(); break;
            case usbKeyboard::end: terminal.moveEnd(); break;
            case usbKeyboard::insert: terminal.overwrite = !terminal.overwrite; break;
            case usbKeyboard::backspace: terminal.backspace(); break;
            case usbKeyboard::del: terminal.erase(); break;
            case usbKeyboard::tab: terminal.insert(U'\t'); break;
            default:
                if (ctrl && key >= 0x04 && key <= 0x1d) terminal.insert(key - 0x03);
                else if (c) terminal.insert(c);
                else return;
        }
    }
    inputDirty = true;
    blinkSince = lv_tick_get();
}

bool repeatable(uint8_t key) {
    return key != usbKeyboard::enter && key != usbKeyboard::keypadEnter && key != usbKeyboard::insert
           && key != usbKeyboard::capsLock && key != usbKeyboard::numLock && key != usbKeyboard::tab
           && key != usbKeyboard::escape && (num || key != 0x62);
}

void pollKeyboard(uint32_t now) {
    static usbKeyboard::Report last{};
    static uint8_t repeating = 0;
    static uint32_t lastRepeat = 0;
    static uint32_t delay = 400;
    usbKeyboard::Report report{};
    while (usbKeyboard::read(report)) {
        for (uint8_t key : report.keys) {
            if (key >= 4 && std::find(last.keys.begin(), last.keys.end(), key) == last.keys.end()) {
                keyboardAction(key, report.modifiers);
                if (repeatable(key)) {
                    repeating = key;
                    lastRepeat = now;
                    delay = 400;
                }
            }
        }
        if (std::find(report.keys.begin(), report.keys.end(), repeating) == report.keys.end()) {
            repeating = 0;
        }
        last = report;
    }
    if (usbKeyboard::connected() && repeating && now - lastRepeat >= delay && repeatable(repeating)) {
        keyboardAction(repeating, last.modifiers);
        lastRepeat = now;
        delay = 60;
    }
}

}

extern "C" void app_main() {
    loadSettings();
    uartReady = startUart();
    if (!view.start()) {
        ESP_LOGE(tag, "Could not initialize HDMI/LVGL");
        return;
    }
    if (!usbKeyboard::start()) {
        ESP_LOGE(tag, "Could not initialize USB keyboard host");
    }
    usbKeyboard::setLockLeds(caps, num);
    uint32_t lastHistory = 0;
    for (;;) {
        const uint32_t now = lv_tick_get();
        receive();
        pollKeyboard(now);
#ifdef STACKTERMINAL_USB_DIAGNOSTICS
        usbKeyboard::Diagnostic diagnostic{};
        while (usbKeyboard::readDiagnostic(diagnostic)) {
            const std::string_view line(diagnostic.data());
            terminal.addLine(std::u32string(line.begin(), line.end()));
            historyDirty = true;
        }
#endif
        const bool renderHistory = historyDirty && (now - lastHistory >= 45 || pages || inputDirty);
        if (inputDirty || renderHistory) {
            char status[180];
            const char parity = "NEO"[settings.parity];
            const char* stop = settings.stop == 0 ? "1" : (settings.stop == 1 ? "1,5" : "2");
            std::snprintf(status, sizeof(status), "UART %lu %lu%c%s%s%s",
                          static_cast<unsigned long>(settings.baud), static_cast<unsigned long>(settings.bits),
                          parity, stop, uartError || !uartReady ? "   |   " : "",
                          uartReady ? (uartError ? uartError : "") : "Błąd UART");
            view.update(terminal, settings, status, renderHistory, pages);
            inputDirty = false;
            pages = 0;
            if (renderHistory) {
                historyDirty = false;
                lastHistory = now;
            }
        }
        view.blink((lv_tick_get() - blinkSince) % 1000 < 500);
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
