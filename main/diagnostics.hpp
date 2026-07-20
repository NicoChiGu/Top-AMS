#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "channel.hpp"
#include "esp_timer.h"
#include "tools.hpp"

namespace diagnostics {

    inline constexpr size_t log_capacity = 64;
    inline constexpr size_t log_batch_size = 16;
    inline constexpr size_t event_code_size = 24;
    inline constexpr size_t event_message_size = 112;
    inline constexpr size_t timeline_detail_size = 64;
    inline constexpr uint64_t mqtt_stale_threshold_ms = 8000;

    inline uint64_t uptime_ms() noexcept {
        return static_cast<uint64_t>(esp_timer_get_time() / 1000);
    }

    enum class level : uint8_t {
        debug,
        info,
        warning,
        error,
    };

    enum class log_module : uint8_t {
        system,
        wifi,
        mqtt,
        web,
        motor,
        filament,
    };

    inline constexpr const char* to_string(level value) noexcept {
        switch (value) {
        case level::debug: return "debug";
        case level::info: return "info";
        case level::warning: return "warning";
        case level::error: return "error";
        }
        return "info";
    }

    inline constexpr const char* to_string(log_module value) noexcept {
        switch (value) {
        case log_module::system: return "system";
        case log_module::wifi: return "wifi";
        case log_module::mqtt: return "mqtt";
        case log_module::web: return "web";
        case log_module::motor: return "motor";
        case log_module::filament: return "filament";
        }
        return "system";
    }

    inline void copy_utf8(char* destination, size_t destination_size, std::string_view source) noexcept {
        if (destination_size == 0)
            return;

        size_t source_index = 0;
        size_t destination_index = 0;
        while (source_index < source.size() && source[source_index] != '\0') {
            const uint8_t first = static_cast<uint8_t>(source[source_index]);
            size_t width = 1;
            if ((first & 0x80U) == 0) {
                width = 1;
            } else if ((first & 0xE0U) == 0xC0U) {
                width = 2;
            } else if ((first & 0xF0U) == 0xE0U) {
                width = 3;
            } else if ((first & 0xF8U) == 0xF0U) {
                width = 4;
            } else {
                if (destination_index + 1 >= destination_size)
                    break;
                destination[destination_index++] = '?';
                source_index++;
                continue;
            }

            if (source_index + width > source.size() || destination_index + width >= destination_size)
                break;

            bool valid = true;
            for (size_t i = 1; i < width; ++i) {
                if ((static_cast<uint8_t>(source[source_index + i]) & 0xC0U) != 0x80U) {
                    valid = false;
                    break;
                }
            }
            if (!valid) {
                if (destination_index + 1 >= destination_size)
                    break;
                destination[destination_index++] = '?';
                source_index++;
                continue;
            }

            std::memcpy(destination + destination_index, source.data() + source_index, width);
            source_index += width;
            destination_index += width;
        }
        destination[destination_index] = '\0';
    }

    struct event {
        uint64_t uptime = 0;
        uint32_t seq = 0;
        level severity = level::info;
        log_module source = log_module::system;
        char code[event_code_size]{};
        // Reserve one extra byte for the terminator so the payload can contain
        // the full 112 bytes promised by the diagnostics protocol.
        char message[event_message_size + 1]{};
    };

    static_assert(log_capacity == 64, "The diagnostics log must stay fixed at 64 entries");
    static_assert(sizeof(event) <= 160, "A diagnostics event must fit in the 10 KiB ring budget");
    static_assert(sizeof(event) * log_capacity <= 10 * 1024,
                  "The diagnostics ring storage must not exceed 10 KiB");

    struct log_batch {
        std::array<event, log_batch_size> events{};
        size_t count = 0;
        size_t total_count = 0;
        size_t offset = 0;
        uint32_t overwritten_count = 0;
    };

    struct log_stats {
        size_t count = 0;
        uint32_t overwritten_count = 0;
    };

    class log_store {
      public:
        event append(level severity, log_module source, std::string_view code, std::string_view message) {
            event value;
            value.uptime = uptime_ms();
            value.severity = severity;
            value.source = source;
            copy_utf8(value.code, sizeof(value.code), code);
            copy_utf8(value.message, sizeof(value.message), message);

            mstd::RAII_lock guard(key_);
            value.seq = next_sequence_++;
            if (count_ < log_capacity) {
                const size_t index = (start_ + count_) % log_capacity;
                events_[index] = value;
                count_++;
            } else {
                events_[start_] = value;
                start_ = (start_ + 1) % log_capacity;
                overwritten_count_++;
            }
            return value;
        }

        log_batch read_batch(size_t offset) {
            log_batch result;
            mstd::RAII_lock guard(key_);
            result.offset = std::min(offset, count_);
            result.total_count = count_;
            result.overwritten_count = overwritten_count_;
            result.count = std::min(log_batch_size, count_ - result.offset);
            for (size_t i = 0; i < result.count; ++i)
                result.events[i] = events_[(start_ + result.offset + i) % log_capacity];
            return result;
        }

        log_stats stats() {
            mstd::RAII_lock guard(key_);
            return log_stats{count_, overwritten_count_};
        }

        uint32_t clear() {
            mstd::RAII_lock guard(key_);
            start_ = 0;
            count_ = 0;
            overwritten_count_ = 0;
            return next_sequence_ - 1;
        }

      private:
        std::array<event, log_capacity> events_{};
        size_t start_ = 0;
        size_t count_ = 0;
        uint32_t next_sequence_ = 1;
        uint32_t overwritten_count_ = 0;
        mstd::lock_key key_;
    };

    using event_sink = void (*)(const event&);
    using reset_sink = void (*)(uint32_t);

    inline log_store logs;
    inline std::atomic<event_sink> on_event{nullptr};
    inline std::atomic<reset_sink> on_reset{nullptr};

    inline void set_log_sinks(event_sink event_callback, reset_sink reset_callback) noexcept {
        on_event.store(event_callback);
        on_reset.store(reset_callback);
    }

    inline event write(level severity, log_module source, std::string_view code, std::string_view message) {
        const event value = logs.append(severity, source, code, message);
        fpr("[", to_string(value.source), "][", to_string(value.severity), "][", value.code, "] ", value.message);
        const event_sink sink = on_event.load();
        if (sink != nullptr)
            sink(value);
        return value;
    }

    inline void clear_logs() {
        const uint32_t cleared_through_seq = logs.clear();
        fpr("[diagnostics] 日志已清空");
        const reset_sink sink = on_reset.load();
        if (sink != nullptr)
            sink(cleared_through_seq);
    }

    enum class mqtt_transport_state : uint8_t {
        not_configured,
        connecting,
        connected,
        disconnected,
        error,
    };

    inline constexpr const char* to_string(mqtt_transport_state value) noexcept {
        switch (value) {
        case mqtt_transport_state::not_configured: return "not_configured";
        case mqtt_transport_state::connecting: return "connecting";
        case mqtt_transport_state::connected: return "connected";
        case mqtt_transport_state::disconnected: return "disconnected";
        case mqtt_transport_state::error: return "error";
        }
        return "not_configured";
    }

    struct mqtt_error {
        bool present = false;
        char kind[24]{};
        int32_t code = 0;
        int32_t detail_code = 0;
        int32_t socket_errno = 0;
        uint64_t at_ms = 0;
        char message[64]{};
    };

    struct mqtt_snapshot {
        mqtt_transport_state transport_state = mqtt_transport_state::not_configured;
        bool stale = false;
        bool has_last_receive = false;
        uint64_t last_receive_ms = 0;
        uint32_t reconnect_count = 0;
        mqtt_error last_error{};
    };

    class mqtt_metrics_store {
      public:
        void mark_not_configured() {
            mstd::RAII_lock guard(key_);
            state_ = mqtt_transport_state::not_configured;
            stale_ = false;
            awaiting_recovery_ = false;
        }

        void mark_connecting() {
            mstd::RAII_lock guard(key_);
            state_ = mqtt_transport_state::connecting;
            stale_ = false;
        }

        void mark_connected() {
            mstd::RAII_lock guard(key_);
            if (ever_connected_)
                reconnect_count_++;
            ever_connected_ = true;
            connected_at_ms_ = uptime_ms();
            state_ = mqtt_transport_state::connected;
            stale_ = false;
        }

        void mark_disconnected() {
            mstd::RAII_lock guard(key_);
            state_ = mqtt_transport_state::disconnected;
            stale_ = false;
        }

        void mark_error(std::string_view kind, int32_t code, int32_t detail_code,
                        int32_t socket_errno, std::string_view message) {
            mstd::RAII_lock guard(key_);
            state_ = mqtt_transport_state::error;
            stale_ = false;
            last_error_.present = true;
            copy_utf8(last_error_.kind, sizeof(last_error_.kind), kind);
            last_error_.code = code;
            last_error_.detail_code = detail_code;
            last_error_.socket_errno = socket_errno;
            last_error_.at_ms = uptime_ms();
            copy_utf8(last_error_.message, sizeof(last_error_.message), message);
        }

        bool mark_received() {
            mstd::RAII_lock guard(key_);
            const bool recovered = awaiting_recovery_;
            last_receive_ms_ = uptime_ms();
            has_last_receive_ = true;
            stale_ = false;
            awaiting_recovery_ = false;
            return recovered;
        }

        bool mark_stale_if_needed(uint64_t threshold_ms = mqtt_stale_threshold_ms) {
            mstd::RAII_lock guard(key_);
            if (state_ != mqtt_transport_state::connected || stale_)
                return false;
            // A prior session's last packet is still useful in the snapshot,
            // but it must not make a freshly reconnected session stale early.
            const uint64_t reference = has_last_receive_
                ? std::max(last_receive_ms_, connected_at_ms_)
                : connected_at_ms_;
            if (uptime_ms() - reference <= threshold_ms)
                return false;
            stale_ = true;
            if (awaiting_recovery_)
                return false;
            awaiting_recovery_ = true;
            return true;
        }

        mqtt_snapshot snapshot() {
            mqtt_snapshot result;
            mstd::RAII_lock guard(key_);
            result.transport_state = state_;
            result.stale = stale_;
            result.has_last_receive = has_last_receive_;
            result.last_receive_ms = last_receive_ms_;
            result.reconnect_count = reconnect_count_;
            result.last_error = last_error_;
            return result;
        }

      private:
        mqtt_transport_state state_ = mqtt_transport_state::not_configured;
        bool stale_ = false;
        bool awaiting_recovery_ = false;
        bool ever_connected_ = false;
        bool has_last_receive_ = false;
        uint64_t connected_at_ms_ = 0;
        uint64_t last_receive_ms_ = 0;
        uint32_t reconnect_count_ = 0;
        mqtt_error last_error_{};
        mstd::lock_key key_;
    };

    inline mqtt_metrics_store mqtt_metrics;

    enum class filament_stage : uint8_t {
        trigger,
        unload,
        retract,
        heat,
        feed,
        resume,
        none,
    };

    inline constexpr size_t filament_stage_count = 6;

    enum class stage_state : uint8_t {
        pending,
        running,
        success,
        skipped,
        warning,
        error,
        not_reached,
    };

    enum class run_state : uint8_t {
        idle,
        running,
        success,
        skipped,
        error,
    };

    inline constexpr const char* to_string(filament_stage value) noexcept {
        switch (value) {
        case filament_stage::trigger: return "trigger";
        case filament_stage::unload: return "unload";
        case filament_stage::retract: return "retract";
        case filament_stage::heat: return "heat";
        case filament_stage::feed: return "feed";
        case filament_stage::resume: return "resume";
        case filament_stage::none: return "none";
        }
        return "none";
    }

    inline constexpr const char* to_string(stage_state value) noexcept {
        switch (value) {
        case stage_state::pending: return "pending";
        case stage_state::running: return "running";
        case stage_state::success: return "success";
        case stage_state::skipped: return "skipped";
        case stage_state::warning: return "warning";
        case stage_state::error: return "error";
        case stage_state::not_reached: return "not_reached";
        }
        return "pending";
    }

    inline constexpr const char* to_string(run_state value) noexcept {
        switch (value) {
        case run_state::idle: return "idle";
        case run_state::running: return "running";
        case run_state::success: return "success";
        case run_state::skipped: return "skipped";
        case run_state::error: return "error";
        }
        return "idle";
    }

    inline constexpr size_t stage_index(filament_stage value) noexcept {
        return static_cast<size_t>(value);
    }

    struct stage_record {
        stage_state state = stage_state::pending;
        uint64_t started_ms = 0;
        uint64_t finished_ms = 0;
        char detail[timeline_detail_size]{};
    };

    struct timeout_record {
        bool present = false;
        filament_stage stage = filament_stage::none;
        uint64_t at_ms = 0;
        int from_channel = 0;
        int to_channel = 0;
        char detail[timeline_detail_size]{};
    };

    struct filament_timeline {
        uint32_t run_id = 0;
        run_state state = run_state::idle;
        int from_channel = 0;
        int to_channel = 0;
        uint64_t started_ms = 0;
        uint64_t finished_ms = 0;
        filament_stage current_stage = filament_stage::none;
        std::array<stage_record, filament_stage_count> stages{};
        timeout_record last_timeout{};
    };

    using timeline_sink = void (*)(const filament_timeline&);
    inline std::atomic<timeline_sink> on_timeline{nullptr};

    inline void set_timeline_sink(timeline_sink callback) noexcept {
        on_timeline.store(callback);
    }

    inline void emit_timeline(const filament_timeline& value) {
        const timeline_sink sink = on_timeline.load();
        if (sink != nullptr)
            sink(value);
    }

    class timeline_store {
      public:
        filament_timeline begin(int from_channel, int to_channel, std::string_view detail) {
            filament_timeline result;
            {
                mstd::RAII_lock guard(key_);
                const timeout_record previous_timeout = value_.last_timeout;
                value_ = filament_timeline{};
                value_.last_timeout = previous_timeout;
                value_.run_id = next_run_id_++;
                value_.state = run_state::running;
                value_.from_channel = from_channel;
                value_.to_channel = to_channel;
                value_.started_ms = uptime_ms();
                value_.current_stage = filament_stage::trigger;
                auto& trigger = value_.stages[stage_index(filament_stage::trigger)];
                trigger.state = stage_state::running;
                trigger.started_ms = value_.started_ms;
                copy_utf8(trigger.detail, sizeof(trigger.detail), detail);
                result = value_;
            }
            emit_timeline(result);
            return result;
        }

        filament_timeline start(filament_stage stage, std::string_view detail) {
            filament_timeline result;
            {
                mstd::RAII_lock guard(key_);
                auto& record = value_.stages[stage_index(stage)];
                record.state = stage_state::running;
                record.started_ms = uptime_ms();
                record.finished_ms = 0;
                copy_utf8(record.detail, sizeof(record.detail), detail);
                value_.current_stage = stage;
                result = value_;
            }
            emit_timeline(result);
            return result;
        }

        filament_timeline finish_stage(filament_stage stage, stage_state state, std::string_view detail) {
            filament_timeline result;
            {
                mstd::RAII_lock guard(key_);
                auto& record = value_.stages[stage_index(stage)];
                if (record.started_ms == 0)
                    record.started_ms = uptime_ms();
                record.state = state;
                record.finished_ms = uptime_ms();
                copy_utf8(record.detail, sizeof(record.detail), detail);
                value_.current_stage = stage;
                result = value_;
            }
            emit_timeline(result);
            return result;
        }

        filament_timeline skip(filament_stage stage, std::string_view detail) {
            return finish_stage(stage, stage_state::skipped, detail);
        }

        filament_timeline warning_timeout(filament_stage stage, std::string_view detail) {
            filament_timeline result;
            {
                mstd::RAII_lock guard(key_);
                auto& record = value_.stages[stage_index(stage)];
                if (record.started_ms == 0)
                    record.started_ms = uptime_ms();
                record.state = stage_state::warning;
                record.finished_ms = uptime_ms();
                copy_utf8(record.detail, sizeof(record.detail), detail);
                remember_timeout_locked(stage, detail);
                value_.current_stage = stage;
                result = value_;
            }
            emit_timeline(result);
            return result;
        }

        filament_timeline fail(filament_stage stage, std::string_view detail, bool timeout) {
            filament_timeline result;
            {
                mstd::RAII_lock guard(key_);
                auto& record = value_.stages[stage_index(stage)];
                if (record.started_ms == 0)
                    record.started_ms = uptime_ms();
                record.state = stage_state::error;
                record.finished_ms = uptime_ms();
                copy_utf8(record.detail, sizeof(record.detail), detail);
                if (timeout)
                    remember_timeout_locked(stage, detail);
                for (size_t i = stage_index(stage) + 1; i < filament_stage_count; ++i) {
                    if (value_.stages[i].state == stage_state::pending)
                        value_.stages[i].state = stage_state::not_reached;
                }
                value_.state = run_state::error;
                value_.finished_ms = uptime_ms();
                value_.current_stage = stage;
                result = value_;
            }
            emit_timeline(result);
            return result;
        }

        filament_timeline finish_run(run_state state) {
            filament_timeline result;
            {
                mstd::RAII_lock guard(key_);
                value_.state = state;
                value_.finished_ms = uptime_ms();
                result = value_;
            }
            emit_timeline(result);
            return result;
        }

        filament_timeline snapshot() {
            mstd::RAII_lock guard(key_);
            return value_;
        }

      private:
        void remember_timeout_locked(filament_stage stage, std::string_view detail) {
            value_.last_timeout.present = true;
            value_.last_timeout.stage = stage;
            value_.last_timeout.at_ms = uptime_ms();
            value_.last_timeout.from_channel = value_.from_channel;
            value_.last_timeout.to_channel = value_.to_channel;
            copy_utf8(value_.last_timeout.detail, sizeof(value_.last_timeout.detail), detail);
        }

        filament_timeline value_{};
        uint32_t next_run_id_ = 1;
        mstd::lock_key key_;
    };

    inline timeline_store filament_history;

} // namespace diagnostics
