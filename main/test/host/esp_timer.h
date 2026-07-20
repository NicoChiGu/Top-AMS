#pragma once

#include <chrono>
#include <cstdint>

extern "C" inline int64_t esp_timer_get_time() {
    static const auto started = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
}
