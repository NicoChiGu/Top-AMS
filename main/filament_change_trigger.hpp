#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace filament_change {

enum class trigger_readiness {
    idle,
    waiting_for_pause,
    waiting_for_channel,
    invalid_channel,
    ready,
    consumed,
};

enum class trigger_kind {
    none,
    filament_change,
    initial_load,
};

struct trigger_request {
    trigger_kind kind = trigger_kind::none;
    int channel = 0;
    bool marker = false;
    bool valid = false;
};

class trigger_state {
  public:
    void reset() {
        bed_target_ = -1;
        gcode_state_ = "UNKNOWN";
        consumed_ = false;
    }

    void update_bed_target(int value) {
        bed_target_ = value;
    }

    void update_gcode_state(std::string_view value) {
        const bool leaving_pause = gcode_state_ == "PAUSE" && value != "PAUSE";
        gcode_state_.assign(value.data(), value.size());
        if (leaving_pause)
            consumed_ = false;
    }

    [[nodiscard]] int bed_target() const noexcept {
        return bed_target_;
    }

    [[nodiscard]] const std::string& gcode_state() const noexcept {
        return gcode_state_;
    }

    [[nodiscard]] trigger_request request(std::size_t channel_count) const noexcept {
        trigger_request result;
        result.marker = bed_target_ > 0 && bed_target_ < 17;
        if (bed_target_ >= 1 && static_cast<std::size_t>(bed_target_) <= channel_count) {
            result.kind = trigger_kind::filament_change;
            result.channel = bed_target_;
            result.valid = true;
        } else if (bed_target_ >= 9 &&
                   static_cast<std::size_t>(bed_target_ - 8) <= channel_count) {
            result.kind = trigger_kind::initial_load;
            result.channel = bed_target_ - 8;
            result.valid = true;
        }
        return result;
    }

    [[nodiscard]] trigger_readiness evaluate(std::size_t channel_count) const noexcept {
        const bool paused = gcode_state_ == "PAUSE";
        const trigger_request decoded = request(channel_count);

        if (consumed_ && paused)
            return trigger_readiness::consumed;
        if (paused && decoded.valid)
            return trigger_readiness::ready;
        if (paused && decoded.marker)
            return trigger_readiness::invalid_channel;
        if (paused)
            return trigger_readiness::waiting_for_channel;
        if (decoded.valid)
            return trigger_readiness::waiting_for_pause;
        return trigger_readiness::idle;
    }

    void consume() noexcept {
        consumed_ = true;
    }

  private:
    int bed_target_ = -1;
    std::string gcode_state_ = "UNKNOWN";
    bool consumed_ = false;
};

} // namespace filament_change
