#include "terminal.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <utility>

namespace {

constexpr size_t historyLimit = 512;

Terminal::Mode cycleMode(Terminal::Mode mode, bool forward) {
    const int count = static_cast<int>(Terminal::Mode::Count);
    const int step = forward ? 1 : count - 1;
    return static_cast<Terminal::Mode>((static_cast<int>(mode) + step) % count);
}

int hexDigit(char32_t character) {
    if (character >= U'0' && character <= U'9') return character - U'0';
    if (character >= U'A' && character <= U'F') return character - U'A' + 10;
    if (character >= U'a' && character <= U'f') return character - U'a' + 10;
    return -1;
}

std::optional<std::string> encodeHex(std::u32string_view draft) {
    std::string bytes;
    int high = -1;
    for (char32_t character : draft) {
        if (character == U' ') continue;
        const int digit = hexDigit(character);
        if (digit < 0) return std::nullopt;
        if (high < 0) {
            high = digit;
        } else {
            bytes += static_cast<char>((high << 4) | digit);
            high = -1;
        }
    }
    if (high >= 0) return std::nullopt;
    return bytes;
}

std::optional<std::string> encodeDecimal(std::u32string_view draft, bool littleEndian) {
    std::string bytes;
    size_t start = draft.find_first_not_of(U' ');
    while (start != std::u32string_view::npos) {
        const size_t end = draft.find(U' ', start);
        const auto word = draft.substr(start, end - start);
        uint64_t value = 0;
        for (char32_t digit : word) {
            if (digit < U'0' || digit > U'9') return std::nullopt;
            value = value * 10 + (digit - U'0');
            if (value > std::numeric_limits<uint32_t>::max()) return std::nullopt;
        }
        constexpr size_t wordSize = sizeof(uint32_t);
        for (size_t i = 0; i < wordSize; ++i) {
            const size_t index = littleEndian ? i : wordSize - 1 - i;
            bytes += static_cast<char>((static_cast<uint32_t>(value) >> (index * 8)) & 0xff);
        }
        start = draft.find_first_not_of(U' ', end);
    }
    return bytes;
}

}

std::string toUtf8(std::u32string_view text) {
    std::string result;
    for (char32_t c : text) {
        if (c < 0x80) {
            result += static_cast<char>(c);
        } else {
            const unsigned bytes = c < 0x800 ? 2 : (c < 0x10000 ? 3 : 4);
            const unsigned shift = 6 * (bytes - 1);
            result += static_cast<char>((0x100u - (1u << (8 - bytes))) | (c >> shift));
            for (unsigned bits = shift; bits > 0; bits -= 6) {
                result += static_cast<char>(0x80 | ((c >> (bits - 6)) & 0x3f));
            }
        }
    }
    return result;
}

bool Terminal::receive(std::string_view bytes) {
    appendRaw(bytes);
    bool changed = false;
    for (unsigned char byte : bytes) {
        if (utf8Left_ > 0) {
            if ((byte & 0xc0) == 0x80) {
                decoded_ = (decoded_ << 6) | (byte & 0x3f);
                if (--utf8Left_ == 0) {
                    const bool valid = decoded_ >= minimum_ && decoded_ <= 0x10ffff
                                       && (decoded_ < 0xd800 || decoded_ > 0xdfff);
                    append(valid ? decoded_ : U'\ufffd');
                    changed = true;
                }
                continue;
            }
            utf8Left_ = 0;
            append(U'\ufffd');
            changed = true;
        }

        if (lastWasCr_ && byte == '\n') {
            if (lastCrLine_ >= firstLine_) {
                lines_[lastCrLine_ - firstLine_] += U'\n';
            }
            lastWasCr_ = false;
            changed = true;
            continue;
        }
        lastWasCr_ = byte == '\r';
        if (byte == '\r' || byte == '\n') {
            lastCrLine_ = firstLine_ + lines_.size();
            finishLine(byte);
        } else if (byte < 0x80) {
            append(byte);
        } else if (byte >= 0xc2 && byte <= 0xf4) {
            utf8Left_ = byte < 0xe0 ? 1 : (byte < 0xf0 ? 2 : 3);
            decoded_ = byte & (0x7f >> (utf8Left_ + 1));
            minimum_ = utf8Left_ == 1 ? 0x80 : (utf8Left_ == 2 ? 0x800 : 0x10000);
            continue;
        } else {
            append(U'\ufffd');
        }
        changed = true;
    }
    return changed || (displayMode_ != Mode::Ascii && !bytes.empty());
}

void Terminal::cycleDisplayMode(bool forward) {
    displayMode_ = cycleMode(displayMode_, forward);
}

void Terminal::cycleSendMode(bool forward) {
    sendMode_ = cycleMode(sendMode_, forward);
}

void Terminal::addRawLine(RawLine line) {
    if (rawLines_.size() == historyLimit) {
        rawLines_.pop_front();
        ++firstRawLine_;
    }
    rawLines_.push_back(std::move(line));
}

void Terminal::appendRaw(std::string_view bytes) {
    auto* block = rawLines_.empty() ? nullptr : std::get_if<RawBytes>(&rawLines_.back());
    while (!bytes.empty()) {
        if (!block || block->size == block->values.size()) {
            addRawLine(RawBytes{});
            block = &std::get<RawBytes>(rawLines_.back());
        }
        const size_t count = std::min(bytes.size(), block->values.size() - block->size);
        std::copy_n(bytes.begin(), count, block->values.begin() + block->size);
        block->size += count;
        bytes.remove_prefix(count);
    }
}

std::u32string Terminal::formatRaw(const RawBytes& bytes) const {
    constexpr char32_t hex[] = U"0123456789ABCDEF";
    constexpr size_t wordSize = sizeof(uint32_t);
    std::u32string result;
    size_t offset = 0;
    const bool decimal = displayMode_ != Mode::Hex;
    if (decimal) {
        const bool littleEndian = displayMode_ == Mode::DecimalLittleEndian;
        for (; offset + wordSize <= bytes.size; offset += wordSize) {
            uint32_t value = 0;
            for (size_t i = 0; i < wordSize; ++i) {
                const size_t index = littleEndian ? wordSize - 1 - i : i;
                value = (value << 8) | bytes.values[offset + index];
            }
            if (!result.empty()) result += U' ';
            char digits[10];
            const auto end = std::to_chars(digits, digits + sizeof(digits), value).ptr;
            result.append(digits, end);
        }
    }
    if (offset == bytes.size) return result;

    if (!result.empty()) result += U' ';
    if (decimal) result += U'[';
    for (size_t i = offset; i < bytes.size; ++i) {
        if (i > offset) result += U' ';
        const uint8_t byte = bytes.values[i];
        result += hex[byte >> 4];
        result += hex[byte & 0x0f];
    }
    if (decimal) result += U']';
    return result;
}

void Terminal::append(char32_t character) {
    if (pending_.size() < 2048) {
        pending_ += character;
    } else if (!truncated_) {
        pending_ += U" [...]";
        truncated_ = true;
    }
}

void Terminal::finishLine(char32_t terminator) {
    if (terminator) {
        pending_ += terminator;
    }
    addLine(std::move(pending_));
    pending_.clear();
    truncated_ = false;
}

void Terminal::addLine(std::u32string line) {
    if (lines_.size() == historyLimit) {
        lines_.pop_front();
        ++firstLine_;
    }
    lines_.push_back(std::move(line));
}

void Terminal::addMessage(std::u32string_view message) {
    addLine(std::u32string(message));
    addRawLine(std::u32string(message));
}

void Terminal::receiveError(std::u32string_view message) {
    if (utf8Left_) {
        append(U'\ufffd');
    }
    utf8Left_ = 0;
    lastWasCr_ = false;
    if (!pending_.empty()) {
        finishLine(0);
    }
    addMessage(message);
}

size_t Terminal::editorIndex() const {
    // Both decimal modes edit the same values and differ only in byte order on send.
    const auto mode = sendMode_ == Mode::DecimalBigEndian ? Mode::DecimalLittleEndian : sendMode_;
    return static_cast<size_t>(mode);
}

void Terminal::Editor::detachHistory() {
    historyPosition = commands.size();
    savedDraft.clear();
}

static bool lineEnd(char32_t character) {
    return character == U'\r' || character == U'\n';
}

void Terminal::insert(char32_t character) {
    if (sendMode_ != Mode::Ascii && character != U' ') {
        const int digit = hexDigit(character);
        if (digit < 0 || (digit >= 10 && sendMode_ != Mode::Hex)) return;
    }

    auto& input = editor();
    if (overwrite && !lineEnd(character) && input.cursor < input.draft.size()
        && !lineEnd(input.draft[input.cursor])) {
        input.draft[input.cursor++] = character;
    } else if (input.draft.size() < commandLimit) {
        input.draft.insert(input.cursor++, 1, character);
    } else {
        return;
    }
    input.detachHistory();
}

void Terminal::insertLineBreak() {
    if (sendMode_ != Mode::Ascii) return;
    auto& input = editor();
    if (input.draft.size() + 2 <= commandLimit) {
        input.draft.insert(input.cursor, U"\r\n");
        input.cursor += 2;
        input.detachHistory();
    }
}

void Terminal::backspace() {
    auto& input = editor();
    if (input.cursor > 0) {
        input.draft.erase(--input.cursor, 1);
        input.detachHistory();
    }
}

void Terminal::erase() {
    auto& input = editor();
    if (input.cursor < input.draft.size()) {
        input.draft.erase(input.cursor, 1);
        input.detachHistory();
    }
}

void Terminal::moveLeft() {
    auto& input = editor();
    if (input.cursor > 0) {
        --input.cursor;
    }
}

void Terminal::moveRight() {
    auto& input = editor();
    if (input.cursor < input.draft.size()) {
        ++input.cursor;
    }
}

void Terminal::moveHome() {
    auto& input = editor();
    while (input.cursor > 0 && !lineEnd(input.draft[input.cursor - 1])) {
        --input.cursor;
    }
}

void Terminal::moveEnd() {
    auto& input = editor();
    while (input.cursor < input.draft.size() && !lineEnd(input.draft[input.cursor])) {
        ++input.cursor;
    }
}

void Terminal::recall(bool older) {
    auto& input = editor();
    if (older && input.historyPosition > 0) {
        if (input.historyPosition == input.commands.size()) {
            input.savedDraft = input.draft;
        }
        --input.historyPosition;
    } else if (!older && input.historyPosition < input.commands.size()) {
        ++input.historyPosition;
    } else {
        return;
    }
    input.draft = input.historyPosition == input.commands.size()
        ? input.savedDraft : input.commands[input.historyPosition];
    input.cursor = input.draft.size();
}

std::optional<std::string> Terminal::transmission(bool appendCrlf) const {
    if (sendMode_ == Mode::Hex) return encodeHex(draft());
    if (sendMode_ != Mode::Ascii) return encodeDecimal(draft(), sendMode_ == Mode::DecimalLittleEndian);

    std::string bytes = toUtf8(draft());
    if (appendCrlf) {
        bytes += "\r\n";
    }
    return bytes;
}

void Terminal::submitted() {
    constexpr size_t commandHistoryLimit = 100;
    auto& input = editor();
    if (!input.draft.empty() && (input.commands.empty() || input.commands.back() != input.draft)) {
        if (input.commands.size() == commandHistoryLimit) {
            input.commands.pop_front();
        }
        input.commands.push_back(std::move(input.draft));
    }
    input.draft.clear();
    input.cursor = 0;
    input.detachHistory();
}
