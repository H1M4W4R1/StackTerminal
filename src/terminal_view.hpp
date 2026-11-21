#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "lvgl.h"
#include "settings.hpp"
#include "terminal.hpp"

class TerminalView {
public:
    bool start();
    void update(const Terminal& terminal, const Settings& settings, const char* status,
                bool refreshHistory, int pages = 0);
    void blink(bool visible);
    void signalRx();
    void signalTx();

    void openSettings(const Settings& current);
    void closeSettings();
    bool settingsOpen() const { return panelOpen_; }
    void selectSetting(int step);
    void adjustSetting(int step);
    void editSetting(char32_t character);
    bool readSettings(Settings& output) const;
    void settingsError(const char* message);

private:
    struct Row {
        lv_obj_t* object = nullptr;
        TerminalView* view = nullptr;
        std::u32string text;
        int cursor = -1;
        bool overwrite = false;
    };

    struct HistoryRow {
        std::u32string_view text;
        uint64_t id;
        size_t offset;
    };

    static void drawRow(lv_event_t* event);
    void assign(Row& row, std::u32string_view text, int cursor = -1, bool overwrite = false);
    void position(Row& row, int x, int y, int width);
    void updateDraft(const Terminal& terminal);
    void updateHistory(const Terminal& terminal, int pages);
    void renderSettings(const char* message = "");
    size_t wrap(std::u32string_view text, int width) const;
    int glyphWidth(char32_t character) const;

    static constexpr size_t settingCount = 6;
    std::array<Row, 44> history_{};
    std::array<Row, 5> editor_{};
    std::vector<HistoryRow> historyRows_;
    std::array<lv_font_t*, 5> fonts_{};
    const lv_font_t* font_ = nullptr;
    Settings theme_;
    lv_obj_t* screen_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_timer_t* rxActivity_ = nullptr;
    lv_timer_t* txActivity_ = nullptr;
    lv_obj_t* prompt_ = nullptr;
    lv_obj_t* editorBackground_ = nullptr;
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* panelPreview_ = nullptr;
    lv_obj_t* panelError_ = nullptr;
    std::array<lv_obj_t*, settingCount> panelRows_{};
    std::array<lv_obj_t*, settingCount> panelValues_{};
    std::array<std::string, settingCount> fields_{};
    Settings draftSettings_;
    uint64_t anchorLine_ = 0;
    size_t anchorOffset_ = 0;
    int historyHeight_ = 0;
    int editorHeight_ = 0;
    int rowHeight_ = 0;
    int cell_ = 0;
    size_t selection_ = 0;
    bool replaceField_ = true;
    bool follow_ = true;
    bool cursorVisible_ = true;
    bool panelOpen_ = false;
};
