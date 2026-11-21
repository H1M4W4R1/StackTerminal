#pragma once

#include <cstdint>

struct Settings {
    uint32_t version = 1;
    uint32_t baud = 115200;
    uint32_t bits = 8;
    uint32_t parity = 0;
    uint32_t stop = 0;
    uint32_t text = 0xe4ebf0;
    uint32_t background = 0x111518;
    uint32_t font = 1;

    bool valid() const {
        return version == 1 && baud >= 300 && baud <= 3000000
               && bits >= 5 && bits <= 8 && parity <= 2 && stop <= 2
               && text <= 0xffffff && background <= 0xffffff && font < 5;
    }
};
