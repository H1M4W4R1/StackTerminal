#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

std::string toUtf8(std::u32string_view text);

class Terminal {
public:
    static constexpr size_t commandLimit = 512;

    bool receive(std::string_view bytes);
    void receiveError(std::u32string_view message);
    void addLine(std::u32string line);
    void insert(char32_t character);
    void insertLineBreak();
    void backspace();
    void erase();
    void moveLeft();
    void moveRight();
    void moveHome();
    void moveEnd();
    void recall(bool older);
    void submitted();

    std::string transmission(bool appendCrlf) const;
    const std::u32string& draft() const { return draft_; }
    size_t cursor() const { return cursor_; }
    bool overwrite = false;

    const std::deque<std::u32string>& lines() const { return lines_; }
    const std::u32string& pending() const { return pending_; }
    uint64_t firstLine() const { return firstLine_; }

private:
    void append(char32_t character);
    void finishLine(char32_t terminator);
    void detachHistory();

    std::deque<std::u32string> lines_;
    std::u32string pending_;
    uint64_t firstLine_ = 0;
    uint64_t lastCrLine_ = 0;
    bool lastWasCr_ = false;
    bool truncated_ = false;
    char32_t decoded_ = 0;
    char32_t minimum_ = 0;
    unsigned utf8Left_ = 0;

    std::u32string draft_;
    size_t cursor_ = 0;
    std::deque<std::u32string> commands_;
    std::u32string savedDraft_;
    size_t historyPosition_ = 0;
};
