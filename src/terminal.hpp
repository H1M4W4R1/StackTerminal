#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

std::string toUtf8(std::u32string_view text);

class Terminal {
public:
    enum class Mode { Ascii, Hex, DecimalLittleEndian, DecimalBigEndian, Count };

    struct RawBytes {
        std::array<uint8_t, 16> values{};
        size_t size = 0;
    };
    using RawLine = std::variant<RawBytes, std::u32string>;

    static constexpr size_t commandLimit = 512;

    bool receive(std::string_view bytes);
    void receiveError(std::u32string_view message);
    void addMessage(std::u32string_view message);
    void cycleDisplayMode(bool forward);
    Mode displayMode() const { return displayMode_; }
    void cycleSendMode(bool forward);
    Mode sendMode() const { return sendMode_; }
    std::u32string formatRaw(const RawBytes& bytes) const;
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

    std::optional<std::string> transmission(bool appendCrlf) const;
    const std::u32string& draft() const { return editor().draft; }
    size_t cursor() const { return editor().cursor; }
    bool overwrite = false;

    const std::deque<std::u32string>& lines() const { return lines_; }
    const std::u32string& pending() const { return pending_; }
    uint64_t firstLine() const { return firstLine_; }
    const std::deque<RawLine>& rawLines() const { return rawLines_; }
    uint64_t firstRawLine() const { return firstRawLine_; }

private:
    struct Editor {
        std::u32string draft;
        size_t cursor = 0;
        std::deque<std::u32string> commands;
        std::u32string savedDraft;
        size_t historyPosition = 0;

        void detachHistory();
    };

    void append(char32_t character);
    void finishLine(char32_t terminator);
    void addLine(std::u32string line);
    void appendRaw(std::string_view bytes);
    void addRawLine(RawLine line);
    size_t editorIndex() const;
    Editor& editor() { return editors_[editorIndex()]; }
    const Editor& editor() const { return editors_[editorIndex()]; }

    Mode displayMode_ = Mode::Ascii;
    Mode sendMode_ = Mode::Ascii;
    std::deque<RawLine> rawLines_;
    uint64_t firstRawLine_ = 0;
    std::deque<std::u32string> lines_;
    std::u32string pending_;
    uint64_t firstLine_ = 0;
    uint64_t lastCrLine_ = 0;
    bool lastWasCr_ = false;
    bool truncated_ = false;
    char32_t decoded_ = 0;
    char32_t minimum_ = 0;
    unsigned utf8Left_ = 0;

    std::array<Editor, 3> editors_;
};
