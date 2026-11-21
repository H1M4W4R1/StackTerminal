#pragma once

#include <cstdint>
#include <array>

namespace usbKeyboard {

enum Key : uint8_t {
    enter = 0x28, escape, backspace, tab, space,
    capsLock = 0x39, insert = 0x49, home, pageUp, del, end, pageDown,
    right, left, down, up, numLock, keypadEnter = 0x58
};

struct Report {
    uint8_t modifiers;
    std::array<uint8_t, 6> keys;
};

inline bool shift(uint8_t modifiers) { return modifiers & 0x22; }
inline bool ctrl(uint8_t modifiers) { return (modifiers & 0x11) && !(modifiers & 0x40); }
inline bool leftAlt(uint8_t modifiers) { return modifiers & 0x04; }
inline bool altGr(uint8_t modifiers) { return modifiers & 0x40; }

bool start();
bool connected();
bool read(Report& report);
#ifdef STACKTERMINAL_USB_DIAGNOSTICS
using Diagnostic = std::array<char, 192>;
bool readDiagnostic(Diagnostic& line);
#endif
void setLockLeds(bool caps, bool num);
char32_t character(uint8_t key, uint8_t modifiers, bool caps, bool num);

}
