#pragma once

#include <atomic>
#include <cstdint>

namespace motor_interlock {

enum class owner : uint8_t {
    none = 0,
    manual,
    unload,
    load,
    assist,
};

inline const char* to_string(owner value) noexcept {
    switch (value) {
    case owner::manual: return "manual";
    case owner::unload: return "unload";
    case owner::load: return "load";
    case owner::assist: return "assist";
    case owner::none:
    default: return "none";
    }
}

class arbiter {
  public:
    bool try_claim(owner requested) noexcept {
        owner expected = owner::none;
        return requested != owner::none &&
               owner_.compare_exchange_strong(expected, requested, std::memory_order_acq_rel);
    }

    void release(owner expected_owner) noexcept {
        owner expected = expected_owner;
        owner_.compare_exchange_strong(expected, owner::none, std::memory_order_acq_rel);
    }

    [[nodiscard]] owner current() const noexcept {
        return owner_.load(std::memory_order_acquire);
    }

    void request_stop() noexcept {
        stop_requested_.store(true, std::memory_order_release);
    }

    void clear_stop_request() noexcept {
        stop_requested_.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return stop_requested_.load(std::memory_order_acquire);
    }

  private:
    std::atomic<owner> owner_{owner::none};
    std::atomic<bool> stop_requested_{false};
};

class falling_edge_debouncer {
  public:
    explicit falling_edge_debouncer(uint8_t required_samples = 5)
        : required_samples_(required_samples == 0 ? 1 : required_samples) {}

    // pressed=true 表示低电平按下。仅在稳定按下边沿返回一次 true。
    bool update(bool pressed) noexcept {
        if (pressed == candidate_) {
            if (candidate_count_ < required_samples_)
                ++candidate_count_;
        } else {
            candidate_ = pressed;
            candidate_count_ = 1;
        }

        if (candidate_count_ < required_samples_)
            return false;

        // 上电时必须先观察到一次稳定释放；微动若卡在低电平，不会误触发送料。
        if (!initialized_) {
            initialized_ = true;
            stable_ = candidate_;
            armed_ = !stable_;
            return false;
        }
        if (stable_ == candidate_)
            return false;

        stable_ = candidate_;
        if (!stable_) {
            armed_ = true;
            return false;
        }
        if (!armed_)
            return false;
        armed_ = false;
        return true;
    }

  private:
    uint8_t required_samples_ = 5;
    uint8_t candidate_count_ = 0;
    bool candidate_ = false;
    bool stable_ = false;
    bool initialized_ = false;
    bool armed_ = false;
};

} // namespace motor_interlock
