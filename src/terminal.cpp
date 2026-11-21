#include "terminal.hpp"

#include <utility>

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
    return changed;
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
    constexpr size_t historyLimit = 512;
    if (lines_.size() == historyLimit) {
        lines_.pop_front();
        ++firstLine_;
    }
    lines_.push_back(std::move(line));
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
    addLine(std::u32string(message));
}

void Terminal::detachHistory() {
    historyPosition_ = commands_.size();
    savedDraft_.clear();
}

static bool lineEnd(char32_t character) {
    return character == U'\r' || character == U'\n';
}

void Terminal::insert(char32_t character) {
    if (overwrite && !lineEnd(character) && cursor_ < draft_.size() && !lineEnd(draft_[cursor_])) {
        draft_[cursor_++] = character;
    } else if (draft_.size() < commandLimit) {
        draft_.insert(cursor_++, 1, character);
    } else {
        return;
    }
    detachHistory();
}

void Terminal::insertLineBreak() {
    if (draft_.size() + 2 <= commandLimit) {
        draft_.insert(cursor_, U"\r\n");
        cursor_ += 2;
        detachHistory();
    }
}

void Terminal::backspace() {
    if (cursor_ > 0) {
        draft_.erase(--cursor_, 1);
        detachHistory();
    }
}

void Terminal::erase() {
    if (cursor_ < draft_.size()) {
        draft_.erase(cursor_, 1);
        detachHistory();
    }
}

void Terminal::moveLeft() {
    if (cursor_ > 0) {
        --cursor_;
    }
}

void Terminal::moveRight() {
    if (cursor_ < draft_.size()) {
        ++cursor_;
    }
}

void Terminal::moveHome() {
    while (cursor_ > 0 && !lineEnd(draft_[cursor_ - 1])) {
        --cursor_;
    }
}

void Terminal::moveEnd() {
    while (cursor_ < draft_.size() && !lineEnd(draft_[cursor_])) {
        ++cursor_;
    }
}

void Terminal::recall(bool older) {
    if (older && historyPosition_ > 0) {
        if (historyPosition_ == commands_.size()) {
            savedDraft_ = draft_;
        }
        --historyPosition_;
    } else if (!older && historyPosition_ < commands_.size()) {
        ++historyPosition_;
    } else {
        return;
    }
    draft_ = historyPosition_ == commands_.size() ? savedDraft_ : commands_[historyPosition_];
    cursor_ = draft_.size();
}

std::string Terminal::transmission(bool appendCrlf) const {
    std::string bytes = toUtf8(draft_);
    if (appendCrlf) {
        bytes += "\r\n";
    }
    return bytes;
}

void Terminal::submitted() {
    constexpr size_t commandHistoryLimit = 100;
    if (!draft_.empty() && (commands_.empty() || commands_.back() != draft_)) {
        if (commands_.size() == commandHistoryLimit) {
            commands_.pop_front();
        }
        commands_.push_back(std::move(draft_));
    }
    draft_.clear();
    cursor_ = 0;
    detachHistory();
}
