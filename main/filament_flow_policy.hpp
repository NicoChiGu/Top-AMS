#pragma once

#include <cstddef>

namespace filament_flow_policy {

inline int confirmed_startup_channel(int saved_channel, bool confirmed,
                                     std::size_t channel_count) noexcept {
    return confirmed && saved_channel >= 1 &&
                   static_cast<std::size_t>(saved_channel) <= channel_count
        ? saved_channel
        : 0;
}

struct resume_evidence {
    bool printer_status_ready = false;
    bool mqtt_fresh = false;
    bool fault_free = false;
    bool sensor_present = false;
    bool channel_confirmed = false;
    int current_channel = 0;
    int target_channel = 0;
};

inline bool can_resume(const resume_evidence& evidence) noexcept {
    return evidence.printer_status_ready && evidence.mqtt_fresh &&
           evidence.fault_free && evidence.sensor_present &&
           evidence.channel_confirmed && evidence.target_channel > 0 &&
           evidence.current_channel == evidence.target_channel;
}

inline int channel_after_attempt(int old_channel, int target_channel,
                                 bool unload_confirmed,
                                 bool feed_confirmed) noexcept {
    if (feed_confirmed)
        return target_channel;
    if (unload_confirmed)
        return 0;
    return old_channel;
}

} // namespace filament_flow_policy
