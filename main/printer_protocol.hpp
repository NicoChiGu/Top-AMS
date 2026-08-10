#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace printer_protocol {

inline std::string normalize_gcode(std::string_view input) {
    std::string normalized;
    normalized.reserve(input.size() + 1);
    for (size_t i = 0; i < input.size(); ++i) {
        const char value = input[i];
        if (value == '\r') {
            normalized.push_back('\n');
            if (i + 1 < input.size() && input[i + 1] == '\n')
                ++i;
        } else {
            normalized.push_back(value);
        }
    }
    while (!normalized.empty() && normalized.back() == '\n')
        normalized.pop_back();
    normalized.push_back('\n');
    return normalized;
}

inline bool parse_sequence_id(std::string_view value, uint32_t& result) noexcept {
    if (value.empty())
        return false;
    uint32_t parsed = 0;
    for (const char digit : value) {
        if (digit < '0' || digit > '9')
            return false;
        const uint32_t number = static_cast<uint32_t>(digit - '0');
        if (parsed > (std::numeric_limits<uint32_t>::max() - number) / 10)
            return false;
        parsed = parsed * 10 + number;
    }
    result = parsed;
    return true;
}

// 只有“状态值发生过变化”形成的新观察才能满足阶段等待；周期 pushall
// 重复发送同一个缓存值不会被误当成本次命令触发的新状态。
inline bool fresh_state_transition_matches(int current, int expected,
                                           uint32_t transition_version,
                                           uint32_t previous_version) noexcept {
    return current == expected && transition_version > previous_version;
}

class sequence_counter {
  public:
    uint32_t next() noexcept {
        uint32_t value = next_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (value == 0)
            value = next_.fetch_add(1, std::memory_order_relaxed) + 1;
        return value;
    }

  private:
    std::atomic<uint32_t> next_{0};
};

class acknowledgement_tracker {
  public:
    void observe(uint32_t sequence) noexcept {
        const uint32_t version = version_.fetch_add(1, std::memory_order_acq_rel) + 1;
        observation& target = observations_[version % observations_.size()];
        target.version.store(0, std::memory_order_release);
        target.sequence.store(sequence, std::memory_order_relaxed);
        target.version.store(version, std::memory_order_release);
    }

    [[nodiscard]] uint32_t version() const noexcept {
        return version_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool observed_after(uint32_t sequence, uint32_t previous_version) const noexcept {
        for (const observation& candidate : observations_) {
            const uint32_t first_version = candidate.version.load(std::memory_order_acquire);
            if (first_version <= previous_version)
                continue;
            const uint32_t candidate_sequence =
                candidate.sequence.load(std::memory_order_relaxed);
            const uint32_t second_version = candidate.version.load(std::memory_order_acquire);
            if (first_version == second_version && candidate_sequence == sequence)
                return true;
        }
        return false;
    }

  private:
    struct observation {
        std::atomic<uint32_t> sequence{0};
        std::atomic<uint32_t> version{0};
    };

    // 保留最近若干回执，避免目标回执刚到又被周期 pushall 的 sequence 0 覆盖。
    std::array<observation, 8> observations_{};
    std::atomic<uint32_t> version_{0};
};

class consecutive_sample_gate {
  public:
    explicit consecutive_sample_gate(uint32_t required_samples,
                                     uint32_t initial_version = 0) noexcept
        : required_samples_(required_samples == 0 ? 1 : required_samples),
          last_version_(initial_version) {}

    bool update(uint32_t sample_version, bool condition) noexcept {
        if (sample_version == last_version_)
            return false;
        last_version_ = sample_version;
        consecutive_ = condition ? consecutive_ + 1 : 0;
        return consecutive_ >= required_samples_;
    }

  private:
    uint32_t required_samples_ = 1;
    uint32_t last_version_ = 0;
    uint32_t consecutive_ = 0;
};

} // namespace printer_protocol
