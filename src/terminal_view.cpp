#include "terminal_view.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <iterator>

#include <M5AtomDisplay.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "src/font/lv_binfont_loader.h"

namespace {

constexpr int width = 1280;
constexpr int height = 720;
constexpr int margin = 28;
constexpr int headerHeight = 52;
constexpr int historyY = 68;
constexpr uint32_t panelColor = 0x1b242c;
constexpr uint32_t dimText = 0x9aadb9;
constexpr uint32_t accent = 0x88d6c3;
constexpr uint32_t separator = 0x35424b;
constexpr int activityDotSize = 10;
constexpr int activityDotGap = 8;
constexpr uint32_t whiteWireColor = 0xffffff;
constexpr uint32_t yellowWireColor = 0xffd338;
constexpr lv_opa_t activityIdleOpacity = LV_OPA_60;
constexpr uint32_t activityPulseMs = 150;

constexpr std::array modeNames{"ASCII", "HEX", "DEC LE (u32)", "DEC BE (u32)"};
static_assert(modeNames.size() == static_cast<size_t>(Terminal::Mode::Count));

constexpr int baudField = 0, textField = 4, backgroundField = 5;
constexpr uint32_t baudRates[]{300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200,
                               230400, 460800, 921600, 1000000, 1500000, 2000000, 3000000};

M5AtomDisplay hdmi(width, height, 60, width, height, 1, 1);

#define EMBEDDED_FONT(size) \
    extern const uint8_t font##size##Start[] asm("_binary_mono_" #size "_bin_start"); \
    extern const uint8_t font##size##End[] asm("_binary_mono_" #size "_bin_end")
EMBEDDED_FONT(16);
EMBEDDED_FONT(20);
EMBEDDED_FONT(24);
EMBEDDED_FONT(30);
EMBEDDED_FONT(36);
#undef EMBEDDED_FONT

std::string_view controlName(char32_t character) {
    constexpr std::string_view ascii[]{
        "NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL",
        "BS", "TAB", "LF", "VT", "FF", "CR", "SO", "SI",
        "DLE", "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB",
        "CAN", "EM", "SUB", "ESC", "FS", "GS", "RS", "US"
    };
    constexpr std::string_view c1[]{
        "PAD", "HOP", "BPH", "NBH", "IND", "NEL", "SSA", "ESA",
        "HTS", "HTJ", "VTS", "PLD", "PLU", "RI", "SS2", "SS3",
        "DCS", "PU1", "PU2", "STS", "CCH", "MW", "SPA", "EPA",
        "SOS", "SGCI", "SCI", "CSI", "ST", "OSC", "PM", "APC"
    };
    if (character < 32) return ascii[character];
    if (character == 127) return "DEL";
    if (character >= 128 && character < 160) return c1[character - 128];
    return {};
}

lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color) {
    auto* object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, w, h);
    if (color) {
        lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    }
    return object;
}

lv_obj_t* label(lv_obj_t* parent, int x, int y, const lv_font_t* font, uint32_t color,
                const char* text) {
    auto* object = lv_label_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_label_set_text(object, text);
    return object;
}

void setBackground(lv_obj_t* object, uint32_t color) {
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
}

lv_timer_t* makeActivityDot(lv_obj_t* parent, int x, uint32_t color) {
    const int y = (headerHeight - activityDotSize) / 2;
    auto* dot = box(parent, x, y, activityDotSize, activityDotSize, color);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, activityIdleOpacity, 0);
    auto* timer = lv_timer_create([](lv_timer_t* expired) {
        auto* dot = static_cast<lv_obj_t*>(lv_timer_get_user_data(expired));
        lv_obj_set_style_bg_opa(dot, activityIdleOpacity, 0);
        lv_timer_pause(expired);
    }, activityPulseMs, dot);
    if (timer) lv_timer_pause(timer);
    return timer;
}

void pulseActivity(lv_timer_t* timer) {
    if (lv_timer_get_paused(timer)) {
        auto* dot = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_timer_resume(timer);
    }
    lv_timer_reset(timer);
}

void setLabel(lv_obj_t* object, const char* text) {
    if (std::strcmp(lv_label_get_text(object), text) != 0) {
        lv_label_set_text(object, text);
    }
}

void setModeLabel(lv_obj_t* object, const char* direction, Terminal::Mode mode) {
    char text[24];
    std::snprintf(text, sizeof(text), "%s %s", direction, modeNames[static_cast<size_t>(mode)]);
    setLabel(object, text);
}

void flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    hdmi.pushImageDMA(area->x1, area->y1, lv_area_get_width(area), lv_area_get_height(area),
                      reinterpret_cast<const lgfx::rgb565_t*>(pixels));
    hdmi.waitDMA();
    lv_display_flush_ready(display);
}

uint32_t tick() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void drawText(lv_layer_t* layer, int x, int y, int w, const lv_font_t* font, uint32_t color,
              const char* text) {
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.color = lv_color_hex(color);
    dsc.text = text;
    dsc.text_local = true;
    const lv_area_t area{x, y, x + w - 1, y + font->line_height - 1};
    lv_draw_label(layer, &dsc, &area);
}

bool parseUnsigned(const std::string& text, uint32_t& value, int base) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return error == std::errc{} && end == text.data() + text.size();
}

std::string hexColor(uint32_t color) {
    char text[7];
    std::snprintf(text, sizeof(text), "%06lX", static_cast<unsigned long>(color));
    return text;
}

}

bool TerminalView::start() {
    if (!hdmi.init()) {
        return false;
    }
    lv_init();
    lv_tick_set_cb(tick);
    auto* display = lv_display_create(width, height);
    constexpr size_t bufferBytes = width * 16 * sizeof(uint16_t);
    auto* buffer = heap_caps_aligned_alloc(32, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!display || !buffer) {
        return false;
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, nullptr, bufferBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_theme(display, nullptr);

    const uint8_t* starts[]{font16Start, font20Start, font24Start, font30Start, font36Start};
    const uint8_t* ends[]{font16End, font20End, font24End, font30End, font36End};
    for (size_t i = 0; i < fonts_.size(); ++i) {
        fonts_[i] = lv_binfont_create_from_buffer(const_cast<uint8_t*>(starts[i]), ends[i] - starts[i]);
        if (!fonts_[i]) return false;
    }
    screen_ = lv_screen_active();
    lv_obj_remove_style_all(screen_);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    box(screen_, 0, 0, width, headerHeight, panelColor);
    status_ = label(screen_, margin, 15, fonts_[0], dimText, "");
    for (std::string_view name : modeNames) {
        modeLabelWidth_ = std::max(modeLabelWidth_, static_cast<int>(name.size()));
    }
    modeLabelWidth_ = (modeLabelWidth_ + 3) * lv_font_get_glyph_width(fonts_[0], 'M', 0);
    const int txDotX = width - margin - activityDotSize;
    const int rxDotX = txDotX - activityDotGap - activityDotSize;
    rxModeLabel_ = label(screen_, rxDotX - 2 * activityDotGap - modeLabelWidth_, 15,
                         fonts_[0], dimText, "RX ASCII");
    lv_obj_set_width(rxModeLabel_, modeLabelWidth_);
    lv_obj_set_style_text_align(rxModeLabel_, LV_TEXT_ALIGN_RIGHT, 0);
    rxActivity_ = makeActivityDot(screen_, rxDotX, whiteWireColor);
    txActivity_ = makeActivityDot(screen_, txDotX, yellowWireColor);
    if (!rxActivity_ || !txActivity_) return false;

    for (auto& row : history_) {
        row.view = this;
        row.object = box(screen_, 0, 0, 1, 1, 0);
        lv_obj_add_flag(row.object, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(row.object, drawRow, LV_EVENT_DRAW_MAIN, &row);
    }

    editorBackground_ = box(screen_, 0, height - 1, width, 1, 0);
    lv_obj_set_style_border_color(editorBackground_, lv_color_hex(separator), 0);
    lv_obj_set_style_border_width(editorBackground_, 1, 0);
    lv_obj_set_style_border_side(editorBackground_, LV_BORDER_SIDE_TOP, 0);
    prompt_ = label(screen_, margin, 0, fonts_[1], accent, ">");
    txModeLabel_ = label(screen_, 0, 0, fonts_[0], dimText, "TX ASCII");
    lv_obj_set_width(txModeLabel_, modeLabelWidth_);
    lv_obj_set_style_text_align(txModeLabel_, LV_TEXT_ALIGN_RIGHT, 0);
    for (auto& row : editor_) {
        row.view = this;
        row.object = box(screen_, 0, 0, 1, 1, 0);
        lv_obj_add_flag(row.object, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(row.object, drawRow, LV_EVENT_DRAW_MAIN, &row);
    }

    panel_ = box(screen_, 0, 0, width, height, 0x10161b);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    label(panel_, 176, 45, fonts_[3], 0xe4ebf0, "Ustawienia terminala");
    constexpr const char* names[]{"Prędkość UART (bit/s)", "Bity danych", "Parzystość", "Bity stopu",
                                   "Kolor tekstu (RRGGBB)", "Kolor tła (RRGGBB)"};
    for (size_t i = 0; i < settingCount; ++i) {
        panelRows_[i] = box(panel_, 176, 119 + 59 * i, 928, 49, panelColor);
        label(panelRows_[i], 18, 10, fonts_[1], 0xe4ebf0, names[i]);
        panelValues_[i] = label(panelRows_[i], 533, 10, fonts_[1], accent, "");
    }
    panelPreview_ = label(panel_, 176, 482, fonts_[1], 0xe4ebf0, "> Podgląd: Zażółć gęślą jaźń");
    lv_obj_set_width(panelPreview_, 928);
    lv_obj_set_style_pad_all(panelPreview_, 12, 0);
    panelError_ = label(panel_, 176, 555, fonts_[0], 0xf5a89f, "");
    return true;
}

int TerminalView::glyphWidth(char32_t character) const {
    const auto name = controlName(character);
    return name.empty() ? cell_ : (static_cast<int>(name.size()) + 1) * cell_;
}

size_t TerminalView::wrap(std::u32string_view text, int available) const {
    size_t count = 0;
    int pixels = 0;
    for (char32_t c : text) {
        pixels += glyphWidth(c);
        if (pixels > available) break;
        ++count;
    }
    return count;
}

void TerminalView::drawRow(lv_event_t* event) {
    const auto& row = *static_cast<Row*>(lv_event_get_user_data(event));
    const auto& view = *row.view;
    auto* layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(row.object, &area);
    int x = area.x1;
    const int y = area.y1 + 3;
    auto text = std::u32string_view(row.text);
    while (!text.empty()) {
        const auto name = controlName(text.front());
        if (!name.empty()) {
            const int w = view.glyphWidth(text.front());
            lv_draw_rect_dsc_t border;
            lv_draw_rect_dsc_init(&border);
            border.bg_opa = LV_OPA_TRANSP;
            border.border_width = 1;
            border.border_color = lv_color_hex(view.theme_.text);
            border.border_opa = LV_OPA_60;
            border.radius = 3;
            const lv_area_t marker{x + 1, area.y1 + 1, x + w - 2, area.y2 - 1};
            lv_draw_rect(layer, &border, &marker);
            drawText(layer, x + view.cell_ / 2, y, w - view.cell_ / 2, view.font_,
                     view.theme_.text, name.data());
            x += w;
            text.remove_prefix(1);
            continue;
        }
        size_t run = 0;
        while (run < text.size() && controlName(text[run]).empty()) ++run;
        const std::string utf8 = toUtf8(text.substr(0, run));
        const int w = static_cast<int>(run) * view.cell_;
        drawText(layer, x, y, w + 2, view.font_, view.theme_.text, utf8.c_str());
        x += w;
        text.remove_prefix(run);
    }
    if (row.cursor >= 0 && view.cursorVisible_) {
        lv_draw_rect_dsc_t cursor;
        lv_draw_rect_dsc_init(&cursor);
        cursor.bg_color = lv_color_hex(view.theme_.text);
        const int left = area.x1 + row.cursor;
        const lv_area_t caret = row.overwrite
            ? lv_area_t{left, area.y2 - 2, left + view.cell_ - 1, area.y2}
            : lv_area_t{left, y, left + 1, area.y2 - 3};
        lv_draw_rect(layer, &cursor, &caret);
    }
}

void TerminalView::position(Row& row, int x, int y, int rowWidth) {
    lv_obj_set_pos(row.object, x, y);
    lv_obj_set_size(row.object, rowWidth, rowHeight_);
    lv_obj_remove_flag(row.object, LV_OBJ_FLAG_HIDDEN);
}

void TerminalView::assign(Row& row, std::u32string_view text, int cursor, bool overwrite) {
    if (row.text != text || row.cursor != cursor || row.overwrite != overwrite) {
        row.text.assign(text);
        row.cursor = cursor;
        row.overwrite = overwrite;
        lv_obj_invalidate(row.object);
    }
}

void TerminalView::updateDraft(const Terminal& terminal) {
    const int x = margin + 2 * cell_;
    const int modeX = width - margin - modeLabelWidth_;
    const int editorWidth = modeX - x - cell_;
    const int available = editorWidth - cell_;
    std::vector<std::u32string> rows;
    std::u32string row;
    int used = 0;
    size_t cursorRow = 0;
    int cursorX = 0;
    for (size_t i = 0; i <= terminal.draft().size(); ++i) {
        const bool atEnd = i == terminal.draft().size();
        const char32_t c = atEnd ? U' ' : terminal.draft()[i];
        const int glyphPixels = glyphWidth(c);
        const bool endsLine = c == U'\n' || (c == U'\r' && (i + 1 == terminal.draft().size()
                                                              || terminal.draft()[i + 1] != U'\n'));
        if (used + glyphPixels > available && !atEnd) {
            rows.push_back(std::move(row));
            row.clear();
            used = 0;
        }
        if (i == terminal.cursor()) {
            cursorRow = rows.size();
            cursorX = used;
        }
        if (!atEnd) {
            row += c;
            used += glyphPixels;
        }
        if (endsLine || atEnd) {
            rows.push_back(std::move(row));
            row.clear();
            used = 0;
        }
    }
    const size_t shown = std::min(rows.size(), editor_.size());
    const size_t first = std::min(rows.size() - shown,
                                  cursorRow >= shown ? cursorRow - shown + 1 : size_t{0});
    editorHeight_ = static_cast<int>(shown) * rowHeight_ + 24;
    const int top = height - editorHeight_ + 12;
    lv_obj_set_pos(editorBackground_, 0, height - editorHeight_);
    lv_obj_set_height(editorBackground_, editorHeight_);
    lv_obj_set_pos(prompt_, margin, top + 3);
    lv_obj_set_pos(txModeLabel_, modeX, top + (rowHeight_ - fonts_[0]->line_height) / 2);
    for (size_t i = 0; i < editor_.size(); ++i) {
        if (i >= shown) {
            lv_obj_add_flag(editor_[i].object, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const size_t index = first + i;
        position(editor_[i], x, top + static_cast<int>(i) * rowHeight_, editorWidth);
        assign(editor_[i], rows[index], index == cursorRow ? cursorX : -1, terminal.overwrite);
    }
}

void TerminalView::updateHistory(const Terminal& terminal, int pages) {
    const int rowWidth = width - 2 * margin;
    historyRows_.clear();
    const bool ascii = displayMode_ == Terminal::Mode::Ascii;
    uint64_t id = ascii ? terminal.firstLine() : terminal.firstRawLine();
    const auto addRows = [&](std::u32string_view line) {
        size_t offset = 0;
        do {
            const size_t count = wrap(line, rowWidth - cell_);
            historyRows_.push_back({line.substr(0, count), id, offset});
            line.remove_prefix(count);
            offset += count;
        } while (!line.empty());
    };
    if (ascii) {
        for (const auto& line : terminal.lines()) {
            addRows(line);
            ++id;
        }
        if (!terminal.pending().empty()) addRows(terminal.pending());
    } else {
        for (const auto& line : terminal.rawLines()) {
            if (const auto* bytes = std::get_if<Terminal::RawBytes>(&line)) {
                // A formatted 16-byte block fits on one row at every supported font size.
                historyRows_.push_back({{}, id, 0, bytes});
            } else {
                addRows(std::get<std::u32string>(line));
            }
            ++id;
        }
    }

    const int visible = std::min(static_cast<int>(history_.size()), historyHeight_ / rowHeight_);
    const int limit = std::max(0, static_cast<int>(historyRows_.size()) - visible);
    int top = limit;
    if (!follow_) {
        const auto anchor = std::upper_bound(historyRows_.begin(), historyRows_.end(),
            std::pair{anchorLine_, anchorOffset_}, [](const auto& a, const HistoryRow& b) {
                return a < std::pair{b.id, b.offset};
            });
        top = std::clamp(static_cast<int>(anchor - historyRows_.begin()) - 1, 0, limit);
    }
    if (pages) {
        top = std::clamp(top + pages * visible, 0, limit);
        follow_ = top == limit;
    }
    if (!historyRows_.empty()) {
        anchorLine_ = historyRows_[top].id;
        anchorOffset_ = historyRows_[top].offset;
    }
    for (size_t i = 0; i < history_.size(); ++i) {
        const size_t index = top + i;
        if (static_cast<int>(i) >= visible || index >= historyRows_.size()) {
            lv_obj_add_flag(history_[i].object, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        position(history_[i], margin, historyY + static_cast<int>(i) * rowHeight_, rowWidth);
        const auto& row = historyRows_[index];
        if (row.bytes) assign(history_[i], terminal.formatRaw(*row.bytes));
        else assign(history_[i], row.text);
    }
    historyRows_.clear();
}

void TerminalView::update(const Terminal& terminal, const Settings& settings, const char* status,
                          bool refreshHistory, int pages) {
    const auto mode = terminal.displayMode();
    const bool changeMode = mode != displayMode_;
    if (changeMode) {
        if (mode == Terminal::Mode::Ascii || displayMode_ == Terminal::Mode::Ascii) {
            follow_ = true;
        }
        displayMode_ = mode;
        setModeLabel(rxModeLabel_, "RX", mode);
    }
    const bool changeTheme = font_ != fonts_[settings.font] || theme_.text != settings.text
                       || theme_.background != settings.background;
    if (changeTheme) {
        theme_ = settings;
        font_ = fonts_[settings.font];
        rowHeight_ = font_->line_height + 6;
        cell_ = lv_font_get_glyph_width(font_, 'M', 0);
        setBackground(screen_, theme_.background);
        setBackground(editorBackground_, theme_.background);
        lv_obj_set_style_text_font(prompt_, font_, 0);
        lv_obj_set_style_text_color(prompt_, lv_color_hex(theme_.text), 0);
        lv_obj_invalidate(screen_);
    }
    setLabel(status_, status);
    setModeLabel(txModeLabel_, "TX", terminal.sendMode());
    updateDraft(terminal);
    const int newHistoryHeight = height - editorHeight_ - historyY - 10;
    if (changeMode || changeTheme || newHistoryHeight != historyHeight_ || refreshHistory || pages) {
        historyHeight_ = newHistoryHeight;
        updateHistory(terminal, pages);
    }
}

void TerminalView::blink(bool visible) {
    if (visible == cursorVisible_) return;
    cursorVisible_ = visible;
    for (auto& row : editor_) {
        if (row.cursor >= 0) lv_obj_invalidate(row.object);
    }
}

void TerminalView::signalRx() { pulseActivity(rxActivity_); }

void TerminalView::signalTx() { pulseActivity(txActivity_); }

void TerminalView::setUartPinsSwapped(bool swapped) {
    auto* rxDot = static_cast<lv_obj_t*>(lv_timer_get_user_data(rxActivity_));
    auto* txDot = static_cast<lv_obj_t*>(lv_timer_get_user_data(txActivity_));
    lv_obj_set_style_bg_color(rxDot, lv_color_hex(swapped ? yellowWireColor : whiteWireColor), 0);
    lv_obj_set_style_bg_color(txDot, lv_color_hex(swapped ? whiteWireColor : yellowWireColor), 0);
}

void TerminalView::openSettings(const Settings& current) {
    draftSettings_ = current;
    fields_[baudField] = std::to_string(current.baud);
    fields_[textField] = hexColor(current.text);
    fields_[backgroundField] = hexColor(current.background);
    selection_ = 0;
    replaceField_ = true;
    panelOpen_ = true;
    lv_obj_remove_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    renderSettings();
}

void TerminalView::closeSettings() {
    panelOpen_ = false;
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
}

void TerminalView::selectSetting(int step) {
    constexpr int count = settingCount;
    selection_ = (static_cast<int>(selection_) + step + count) % count;
    replaceField_ = true;
    renderSettings();
}

void TerminalView::adjustSetting(int step) {
    switch (selection_) {
        case baudField: {
            uint32_t value = draftSettings_.baud;
            parseUnsigned(fields_[baudField], value, 10);
            const auto begin = std::begin(baudRates);
            const auto end = std::end(baudRates);
            if (step > 0) {
                fields_[baudField] = std::to_string(*std::min(std::upper_bound(begin, end, value), end - 1));
            } else {
                auto lower = std::lower_bound(begin, end, value);
                fields_[baudField] = std::to_string(*(lower == begin ? begin : lower - 1));
            }
            replaceField_ = true;
            break;
        }
        case 1: draftSettings_.bits = 5 + (draftSettings_.bits - 5 + step + 4) % 4; break;
        case 2: draftSettings_.parity = (draftSettings_.parity + step + 3) % 3; break;
        case 3: draftSettings_.stop = (draftSettings_.stop + step + 3) % 3; break;
        default: return;
    }
    renderSettings();
}

void TerminalView::editSetting(char32_t character) {
    const bool decimal = selection_ == baudField;
    if (!decimal && selection_ != textField && selection_ != backgroundField) return;
    auto& field = fields_[selection_];
    if (character == U'\b') {
        if (!field.empty()) field.pop_back();
        replaceField_ = false;
    } else if ((character >= U'0' && character <= U'9')
               || (!decimal && ((character >= U'A' && character <= U'F')
                                || (character >= U'a' && character <= U'f')))) {
        if (replaceField_) field.clear();
        replaceField_ = false;
        if (field.size() < (decimal ? 7 : 6)) field += static_cast<char>(character);
    }
    renderSettings();
}

bool TerminalView::readSettings(Settings& output) const {
    output = draftSettings_;
    return fields_[textField].size() == 6 && fields_[backgroundField].size() == 6
           && parseUnsigned(fields_[baudField], output.baud, 10)
           && parseUnsigned(fields_[textField], output.text, 16)
           && parseUnsigned(fields_[backgroundField], output.background, 16) && output.valid();
}

void TerminalView::settingsError(const char* message) { renderSettings(message); }

void TerminalView::renderSettings(const char* message) {
    fields_[1] = std::to_string(draftSettings_.bits);
    fields_[2] = std::array{"Brak", "Parzysta", "Nieparzysta"}[draftSettings_.parity];
    fields_[3] = std::array{"1", "1,5", "2"}[draftSettings_.stop];
    for (size_t i = 0; i < settingCount; ++i) {
        setBackground(panelRows_[i], i == selection_ ? 0x30424e : panelColor);
        setLabel(panelValues_[i], fields_[i].c_str());
    }
    uint32_t fg = draftSettings_.text, bg = draftSettings_.background;
    if (fields_[textField].size() == 6) parseUnsigned(fields_[textField], fg, 16);
    if (fields_[backgroundField].size() == 6) parseUnsigned(fields_[backgroundField], bg, 16);
    lv_obj_set_style_text_color(panelPreview_, lv_color_hex(fg), 0);
    setBackground(panelPreview_, bg);
    setLabel(panelError_, message);
}
