#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <semaphore>
#include <string_view>
#include <thread>

#include "esptools.hpp"
#include "filament_change_trigger.hpp"
#include "filament_flow_policy.hpp"
#include "motor_interlock.hpp"
#include "printer_protocol.hpp"
#include "printer_status.hpp"

#include "bambu.hpp"
#include "espIO.hpp"
#include "espMQTT.hpp"

#include "channel.hpp"
#include "diagnostics.hpp"

#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "web_sync.hpp"

#if __has_include("localconfig.hpp")
// #include "localconfig.hpp"//发布时去掉这个
#endif


using std::string;

//新添加的变量
TaskHandle_t Task1_handle;//微动任务
// -1=尚未收到真实状态，0=无料，1=有料。
std::atomic<int> hw_switch{-1};
TaskHandle_t Task2_handle;//延时调试信息


//分割

std::atomic<int> bed_target_temper_max{0};
mesp::wsStoreValue<int> extruder("extruder", 0);// 1-active_motor_count，0 表示未装载或尚未确认
mesp::wsStoreValue<bool> extruder_confirmed("extruder_confirmed", false);

std::atomic<uint32_t> print_error{0};
std::atomic<int> ams_status = -1;
std::atomic<uint32_t> ams_status_version{0};
std::atomic<uint32_t> ams_status_transition_version{0};
std::atomic<bool> pause_lock{false};// 暂停锁
filament_change::trigger_state change_trigger;
filament_change::trigger_readiness last_change_trigger_readiness = filament_change::trigger_readiness::idle;
bool change_trigger_busy_logged = false;
std::atomic<int> nozzle_target_temper = -1;
std::atomic<float> nozzle_actual_temper{-1.0f};
std::atomic<uint32_t> nozzle_actual_version{0};
std::atomic<bool> printer_status_ready{false};
std::atomic<uint64_t> printer_status_updated_ms{0};
printer_protocol::sequence_counter command_sequences;
printer_protocol::acknowledgement_tracker command_acks;

// 系统状态变量，用于前端显示和按钮控制
mesp::wsValue<int> motor_running("motor_running", 0);// 当前运行的电机通道，0表示无电机运行
mesp::wsValue<string> operation_status("operation_status", "idle");// 操作状态: "idle", "changing", "loading"
mesp::wsValue<bool> system_locked("system_locked", false);// 系统锁定状态
mesp::wsValue<string> motor_owner("motor_owner", "none");
mesp::wsValue<string> flow_phase("flow_phase", "idle");
std::atomic<int> pending_feed_channel{0};
std::atomic<bool> flow_abort_requested{false};
motor_interlock::arbiter motor_arbiter;

class printer_fault_store {
  public:
    bool update_print_error(uint32_t code) {
        mstd::RAII_lock guard(key_);
        const auto before = current_;
        print_error_ = printer_status::decode_print_error(code);
        recompute_locked();
        return current_ != before;
    }

    bool update_hms(const printer_status::fault_info& value) {
        mstd::RAII_lock guard(key_);
        const auto before = current_;
        hms_ = value;
        recompute_locked();
        return current_ != before;
    }

    printer_status::fault_info snapshot() {
        mstd::RAII_lock guard(key_);
        return current_;
    }

  private:
    void recompute_locked() {
        if (print_error_.active)
            current_ = print_error_;
        else if (hms_.active)
            current_ = hms_;
        else
            current_ = {};
    }

    mstd::lock_key key_;
    printer_status::fault_info print_error_;
    printer_status::fault_info hms_;
    printer_status::fault_info current_;
};

printer_fault_store printer_faults;

AsyncWebServer server(80);
// AsyncWebSocket ws("/ws");
AsyncWebSocket& ws = mesp::ws_server;//先直接用全局的ws_server

inline mstd::channel_lock<std::function<void()>> async_channel;//异步任务通道

// RAII状态管理类，确保在异常或提前返回时也能清理状态
struct SystemStateGuard {
    bool active;
    bool faulted;
    SystemStateGuard() : active(true), faulted(false) {
        system_locked = true;
    }
    ~SystemStateGuard() {
        if (active) {
            pending_feed_channel.store(0);
            system_locked = false;
            operation_status = "idle";
            pause_lock = false;
            if (!faulted)
                flow_phase = "idle";
        }
    }
    void release() {
        if (!active)
            return;
        pending_feed_channel.store(0);
        system_locked = false;
        operation_status = "idle";
        pause_lock = false;
        if (!faulted)
            flow_phase = "idle";
        active = false;
    }
    void fail() {
        faulted = true;
        flow_phase = "fault";
        release();
    }
};


inline void filament_log(diagnostics::level severity, std::string_view code, const string& str) {
    diagnostics::write(severity, diagnostics::log_module::filament, code, str);
}

inline bool valid_channel(int channel) noexcept {
    return channel >= 1 && static_cast<size_t>(channel) <= config::active_motor_count;
}

inline void set_current_channel(int channel, bool confirmed) {
    if (channel != 0 && !valid_channel(channel))
        return;
    extruder = channel;
    extruder.update();
    extruder_confirmed = channel != 0 && confirmed;
    extruder_confirmed.update();
}

inline void stop_all_motor_gpio() {
    for (size_t index = 0; index < config::active_motor_count; ++index) {
        esp::gpio_out(config::motors[index].forward, false);
        esp::gpio_out(config::motors[index].backward, false);
    }
}

inline void emergency_stop_motors(std::string_view reason) {
    motor_arbiter.request_stop();
    stop_all_motor_gpio();
    motor_running = 0;
    diagnostics::write(diagnostics::level::error, diagnostics::log_module::motor,
                       "MOTOR_EMERGENCY_STOP", std::string(reason));
}

inline bool mqtt_is_unsafe() {
    const auto snapshot = diagnostics::mqtt_metrics.snapshot();
    return snapshot.transport_state != diagnostics::mqtt_transport_state::connected ||
           snapshot.stale;
}

inline bool flow_should_abort() {
    const auto fault = printer_faults.snapshot();
    return flow_abort_requested.load() || mqtt_is_unsafe() ||
           (fault.active && fault.interlock);
}

inline void fault_to_json(JsonObject target, const printer_status::fault_info& fault) {
    target["active"] = fault.active;
    target["interlock"] = fault.interlock;
    target["known"] = fault.known;
    target["source"] = fault.source;
    target["raw_code"] = fault.raw_code;
    target["display_code"] = fault.display_code;
    target["label_zh"] = fault.label_zh;
    target["action_zh"] = fault.action_zh;
    target["severity"] = printer_status::to_string(fault.severity);
}

inline void printer_operation_to_json(JsonObject target) {
    const auto decoded = printer_status::decode_ams_status(ams_status.load());
    const auto fault = printer_faults.snapshot();
    const int sensor = hw_switch.load();
    const int current_channel = extruder.get_value();
    const bool confirmed = extruder_confirmed.get_value() && valid_channel(current_channel);

    JsonObject ams = target["ams_status_info"].to<JsonObject>();
    ams["raw"] = decoded.raw;
    ams["main"] = decoded.main;
    ams["sub"] = decoded.sub;
    ams["label_zh"] = decoded.label_zh;
    ams["known"] = decoded.known;

    JsonObject printer_fault = target["printer_fault"].to<JsonObject>();
    fault_to_json(printer_fault, fault);
    target["printer_status_ready"] = printer_status_ready.load();
    target["channel_confirmed"] = confirmed;
    target["channel_confirmation_required"] =
        printer_status_ready.load() && sensor == 1 && !confirmed;
    target["hw_switch"] = sensor;
    target["current_channel"] = current_channel;
    target["nozzle_actual_c"] = nozzle_actual_temper.load();
    target["motor_owner"] = motor_owner.get_value();
    target["flow_phase"] = flow_phase.get_value();
    target["updated_ms"] = printer_status_updated_ms.load();
}

inline void send_printer_operation_status(AsyncWebSocketClient* client = nullptr) {
    JsonDocument doc;
    doc["type"] = "printer_operation_status";
    JsonObject payload = doc["payload"].to<JsonObject>();
    printer_operation_to_json(payload);
    String msg;
    serializeJson(doc, msg);
    if (client != nullptr)
        client->text(msg);
    else
        ws.textAll(msg);
}

inline const char* reset_reason_label(esp_reset_reason_t reason) {
    switch (reason) {
    case ESP_RST_POWERON: return "上电启动";
    case ESP_RST_EXT: return "外部复位";
    case ESP_RST_SW: return "软件重启";
    case ESP_RST_PANIC: return "异常崩溃";
    case ESP_RST_INT_WDT: return "中断看门狗";
    case ESP_RST_TASK_WDT: return "任务看门狗";
    case ESP_RST_WDT: return "其他看门狗";
    case ESP_RST_DEEPSLEEP: return "深度睡眠唤醒";
    case ESP_RST_BROWNOUT: return "电压过低";
    case ESP_RST_SDIO: return "SDIO 复位";
    case ESP_RST_USB: return "USB 复位";
    case ESP_RST_JTAG: return "JTAG 复位";
    case ESP_RST_EFUSE: return "eFuse 错误复位";
    case ESP_RST_PWR_GLITCH: return "电源毛刺复位";
    case ESP_RST_CPU_LOCKUP: return "CPU 锁死复位";
    case ESP_RST_UNKNOWN:
    default: return "未知原因";
    }
}

inline void event_to_json(JsonObject target, const diagnostics::event& value) {
    target["seq"] = value.seq;
    target["uptime_ms"] = value.uptime;
    target["level"] = diagnostics::to_string(value.severity);
    target["module"] = diagnostics::to_string(value.source);
    target["code"] = value.code;
    target["message"] = value.message;
}

inline void timeline_to_json(JsonObject target, const diagnostics::filament_timeline& value) {
    target["run_id"] = value.run_id;
    target["state"] = diagnostics::to_string(value.state);
    target["from_channel"] = value.from_channel;
    target["to_channel"] = value.to_channel;
    target["started_ms"] = value.started_ms;
    if (value.finished_ms > 0)
        target["finished_ms"] = value.finished_ms;
    else
        target["finished_ms"] = nullptr;
    target["current_stage"] = diagnostics::to_string(value.current_stage);

    JsonArray stages = target["stages"].to<JsonArray>();
    constexpr std::array<diagnostics::filament_stage, diagnostics::filament_stage_count> stage_order{
        diagnostics::filament_stage::trigger,
        diagnostics::filament_stage::unload,
        diagnostics::filament_stage::retract,
        diagnostics::filament_stage::heat,
        diagnostics::filament_stage::feed,
        diagnostics::filament_stage::resume,
    };
    for (size_t i = 0; i < stage_order.size(); ++i) {
        JsonObject stage = stages.add<JsonObject>();
        stage["id"] = diagnostics::to_string(stage_order[i]);
        stage["state"] = diagnostics::to_string(value.stages[i].state);
        if (value.stages[i].started_ms > 0)
            stage["started_ms"] = value.stages[i].started_ms;
        else
            stage["started_ms"] = nullptr;
        if (value.stages[i].finished_ms > 0)
            stage["finished_ms"] = value.stages[i].finished_ms;
        else
            stage["finished_ms"] = nullptr;
        stage["detail"] = value.stages[i].detail;
    }

    if (value.last_timeout.present) {
        JsonObject timeout = target["last_timeout"].to<JsonObject>();
        timeout["stage"] = diagnostics::to_string(value.last_timeout.stage);
        timeout["at_ms"] = value.last_timeout.at_ms;
        timeout["from_channel"] = value.last_timeout.from_channel;
        timeout["to_channel"] = value.last_timeout.to_channel;
        timeout["detail"] = value.last_timeout.detail;
    } else {
        target["last_timeout"] = nullptr;
    }
}

inline void send_diagnostics_event(const diagnostics::event& value) {
    JsonDocument doc;
    doc["type"] = "diagnostics_event";
    JsonObject payload = doc["payload"].to<JsonObject>();
    event_to_json(payload, value);
    payload["overwritten_count"] = diagnostics::logs.stats().overwritten_count;
    String msg;
    serializeJson(doc, msg);
    ws.textAll(msg);
}

inline void send_diagnostics_log_reset(uint32_t cleared_through_seq) {
    JsonDocument doc;
    doc["type"] = "diagnostics_log_reset";
    JsonObject payload = doc["payload"].to<JsonObject>();
    payload["cleared_through_seq"] = cleared_through_seq;
    String msg;
    serializeJson(doc, msg);
    ws.textAll(msg);
}

inline void broadcast_filament_timeline(const diagnostics::filament_timeline& value) {
    JsonDocument doc;
    doc["type"] = "filament_timeline";
    JsonObject payload = doc["payload"].to<JsonObject>();
    timeline_to_json(payload, value);
    String msg;
    serializeJson(doc, msg);
    ws.textAll(msg);
}

inline void send_filament_timeline(AsyncWebSocketClient* client) {
    if (client == nullptr)
        return;
    JsonDocument doc;
    doc["type"] = "filament_timeline";
    JsonObject payload = doc["payload"].to<JsonObject>();
    timeline_to_json(payload, diagnostics::filament_history.snapshot());
    String msg;
    serializeJson(doc, msg);
    client->text(msg);
}

inline void send_diagnostics_log_history(AsyncWebSocketClient* client) {
    if (client == nullptr)
        return;

    size_t offset = 0;
    bool first_batch = true;
    while (true) {
        const diagnostics::log_batch batch = diagnostics::logs.read_batch(offset);
        JsonDocument doc;
        doc["type"] = "diagnostics_log_batch";
        JsonObject payload = doc["payload"].to<JsonObject>();
        payload["reset"] = first_batch;
        payload["overwritten_count"] = batch.overwritten_count;
        payload["total_count"] = batch.total_count;
        JsonArray events = payload["events"].to<JsonArray>();
        for (size_t i = 0; i < batch.count; ++i) {
            JsonObject item = events.add<JsonObject>();
            event_to_json(item, batch.events[i]);
        }
        offset += batch.count;
        payload["has_more"] = offset < batch.total_count;
        String msg;
        serializeJson(doc, msg);
        client->text(msg);
        first_batch = false;
        if (batch.count == 0 || offset >= batch.total_count)
            break;
    }
}

inline void send_diagnostics_snapshot(AsyncWebSocketClient* client) {
    if (client == nullptr)
        return;

    const uint64_t now_ms = diagnostics::uptime_ms();
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    const diagnostics::mqtt_snapshot mqtt_snapshot = diagnostics::mqtt_metrics.snapshot();
    const bool wifi_connected = WiFi.status() == WL_CONNECTED;

    JsonDocument doc;
    doc["type"] = "diagnostics_snapshot";
    JsonObject payload = doc["payload"].to<JsonObject>();
    payload["firmware_version"] = esp_app_get_description()->version;
    payload["uptime_ms"] = now_ms;

    JsonObject reset = payload["reset_reason"].to<JsonObject>();
    reset["code"] = static_cast<int>(reset_reason);
    reset["label"] = reset_reason_label(reset_reason);

    JsonObject heap = payload["heap"].to<JsonObject>();
    heap["free_bytes"] = esp_get_free_heap_size();
    heap["minimum_free_bytes"] = esp_get_minimum_free_heap_size();

    JsonObject wifi = payload["wifi"].to<JsonObject>();
    wifi["connected"] = wifi_connected;
    wifi["ssid"] = wifi_connected ? WiFi.SSID() : String();
    wifi["ip"] = wifi_connected ? WiFi.localIP().toString() : String();
    if (wifi_connected)
        wifi["rssi_dbm"] = WiFi.RSSI();
    else
        wifi["rssi_dbm"] = nullptr;

    JsonObject mqtt = payload["mqtt"].to<JsonObject>();
    mqtt["transport_state"] = diagnostics::to_string(mqtt_snapshot.transport_state);
    mqtt["stale"] = mqtt_snapshot.stale;
    mqtt["reconnect_count"] = mqtt_snapshot.reconnect_count;
    if (mqtt_snapshot.has_last_receive) {
        mqtt["last_receive_ms"] = mqtt_snapshot.last_receive_ms;
        mqtt["last_receive_age_ms"] = now_ms - mqtt_snapshot.last_receive_ms;
    } else {
        mqtt["last_receive_ms"] = nullptr;
        mqtt["last_receive_age_ms"] = nullptr;
    }
    if (mqtt_snapshot.last_error.present) {
        JsonObject error = mqtt["last_error"].to<JsonObject>();
        error["kind"] = mqtt_snapshot.last_error.kind;
        error["code"] = mqtt_snapshot.last_error.code;
        error["detail_code"] = mqtt_snapshot.last_error.detail_code;
        error["socket_errno"] = mqtt_snapshot.last_error.socket_errno;
        error["at_ms"] = mqtt_snapshot.last_error.at_ms;
        error["message"] = mqtt_snapshot.last_error.message;
    } else {
        mqtt["last_error"] = nullptr;
    }

    payload["websocket_clients"] = ws.count();
    JsonObject system = payload["system"].to<JsonObject>();
    system["operation_status"] = operation_status.get_value();
    system["locked"] = system_locked.get_value();
    system["motor_running"] = motor_running.get_value();
    system["ams_status"] = ams_status.load();
    system["nozzle_target_c"] = nozzle_target_temper.load();
    printer_operation_to_json(system);

    String msg;
    serializeJson(doc, msg);
    client->text(msg);
}

inline void mqtt_state_changed(int state) {
    if (state != mesp::Mqttclient::mqtt_state::connected) {
        change_trigger.reset();
        last_change_trigger_readiness = filament_change::trigger_readiness::idle;
        change_trigger_busy_logged = false;
    }
    if (state == mesp::Mqttclient::mqtt_state::disconnected ||
        state == mesp::Mqttclient::mqtt_state::error) {
        printer_status_ready.store(false);
        printer_status_updated_ms.store(diagnostics::uptime_ms());
        flow_abort_requested.store(true);
        emergency_stop_motors("MQTT 连接中断，已停止全部电机");
        flow_phase = "fault";
        send_printer_operation_status();
    }
    config::MQTT_done = state == mesp::Mqttclient::mqtt_state::connected;
}

// @brief 将当前Wi-Fi运行状态发送给指定WebSocket客户端
// @note 该数据只读且不持久化；未连接时清空网络字段，避免前端显示旧值
inline void send_wifi_status(AsyncWebSocketClient* client) {
    if (client == nullptr)
        return;

    const bool connected = WiFi.status() == WL_CONNECTED;
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    JsonArray data = root.createNestedArray("data");

    JsonObject connected_item = data.createNestedObject();
    connected_item["name"] = "wifi_connected";
    connected_item["value"] = connected;

    JsonObject ssid_item = data.createNestedObject();
    ssid_item["name"] = "wifi_ssid";
    ssid_item["value"] = connected ? WiFi.SSID() : String();

    JsonObject ip_item = data.createNestedObject();
    ip_item["name"] = "wifi_ip";
    String local_ip_text;
    if (connected) {
        const IPAddress local_ip = WiFi.localIP();
        const bool has_local_ip = local_ip[0] != 0 || local_ip[1] != 0 ||
                                  local_ip[2] != 0 || local_ip[3] != 0;
        if (has_local_ip)
            local_ip_text = local_ip.toString();
    }
    ip_item["value"] = local_ip_text;

    JsonObject rssi_item = data.createNestedObject();
    rssi_item["name"] = "wifi_rssi";
    if (connected)
        rssi_item["value"] = WiFi.RSSI();
    else
        rssi_item["value"] = nullptr;

    String msg;
    serializeJson(doc, msg);
    client->text(msg);
}

// @brief 判断WebSocket配置项是否为电机反向输出设置
inline bool is_motor_reverse_setting(const std::string& name) {
    constexpr char suffix[] = "_reverse";
    constexpr size_t suffix_size = sizeof(suffix) - 1;
    return name.rfind("ext", 0) == 0 &&
           name.size() > suffix_size &&
           name.compare(name.size() - suffix_size, suffix_size, suffix) == 0;
}

// @brief 将单个WebSocket配置项的当前值重新同步到前端
inline void sync_ws_value(const std::string& name) {
    auto it = mesp::ws_value_to_json.find(name);
    if (it == mesp::ws_value_to_json.end())
        return;

    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    root.createNestedArray("data");
    it->second(doc);
    mesp::sendJson(doc);
}

// @brief 电机方向配置只能在系统完全空闲时修改
inline bool can_update_motor_direction() {
    return !system_locked.get_value() &&
           !pause_lock.load() &&
           operation_status.get_value() == "idle" &&
           motor_running.get_value() == 0 &&
           motor_arbiter.current() == motor_interlock::owner::none;
}

enum class motor_run_result {
    completed,
    busy,
    invalid_channel,
    stopped,
    timed_out,
};

struct MotorRunGuard {
    config::motor& motor;
    motor_interlock::owner owner;

    ~MotorRunGuard() {
        esp::gpio_out(motor.forward, false);
        esp::gpio_out(motor.backward, false);
        motor_running = 0;
        motor_owner = "none";
        motor_arbiter.release(owner);
    }
};

inline motor_run_result motor_run_controlled(
    int channel, bool forward, std::chrono::milliseconds maximum_duration,
    motor_interlock::owner owner,
    const std::function<bool()>& completion = {}, bool require_completion = false,
    std::chrono::milliseconds handoff = 0ms) {
    if (!valid_channel(channel) || maximum_duration.count() <= 0) {
        diagnostics::write(diagnostics::level::error, diagnostics::log_module::motor,
                           "MOTOR_INVALID_CHANNEL",
                           "电机通道或运行时间无效: " + std::to_string(channel));
        return motor_run_result::invalid_channel;
    }

    if (!motor_arbiter.try_claim(owner)) {
        diagnostics::write(diagnostics::level::warning, diagnostics::log_module::motor,
                           "MOTOR_BUSY", "电机已由 " + motor_owner.get_value() + " 占用");
        return motor_run_result::busy;
    }

    const int motor_index = channel - 1;
    if (config::motors[motor_index].forward == config::LED_R) [[unlikely]] {
        config::LED_R = GPIO_NUM_NC;
        config::LED_L = GPIO_NUM_NC;
    }//使用到了通道7,关闭代码中的LED控制

    auto& motor = config::motors[motor_index];
    MotorRunGuard cleanup{motor, owner};

    motor_running = channel;
    motor_owner = motor_interlock::to_string(owner);

    // 运行开始后快照配置，确保一次动作始终使用同一个物理方向。
    const bool reverse_output = motor.reverse_output.get_value();
    const bool physical_forward = forward != reverse_output;
    const gpio_num_t active_gpio = physical_forward ? motor.forward : motor.backward;
    const gpio_num_t inactive_gpio = physical_forward ? motor.backward : motor.forward;

    diagnostics::write(diagnostics::level::info, diagnostics::log_module::motor,
                       "MOTOR_STARTED", std::string("电机 ") + std::to_string(channel) +
                       (forward ? " 正转" : " 反转") + "，owner=" +
                       motor_interlock::to_string(owner) +
                       (reverse_output ? "（反向输出）" : ""));

    esp::gpio_out(inactive_gpio, false);
    esp::gpio_out(active_gpio, true);

    const auto deadline = std::chrono::steady_clock::now() + maximum_duration;
    const auto condition_deadline = completion && handoff.count() > 0
        ? deadline - std::min(maximum_duration, handoff)
        : deadline;
    bool condition_met = false;
    while (std::chrono::steady_clock::now() < condition_deadline) {
        if (motor_arbiter.stop_requested() || flow_should_abort()) {
            diagnostics::write(diagnostics::level::error, diagnostics::log_module::motor,
                               "MOTOR_STOPPED", "安全联锁停止电机 " + std::to_string(channel));
            return motor_run_result::stopped;
        }
        if (completion && completion()) {
            condition_met = true;
            break;
        }
        mstd::delay(20ms);
    }

    if (condition_met && handoff.count() > 0) {
        const auto handoff_deadline = std::chrono::steady_clock::now() + handoff;
        while (std::chrono::steady_clock::now() < handoff_deadline) {
            if (motor_arbiter.stop_requested() || flow_should_abort())
                return motor_run_result::stopped;
            mstd::delay(20ms);
        }
    }

    if (require_completion && !condition_met) {
        diagnostics::write(diagnostics::level::error, diagnostics::log_module::motor,
                           "MOTOR_SENSOR_TIMEOUT",
                           "电机 " + std::to_string(channel) + " 达到最大进料时间但传感器未确认");
        return motor_run_result::timed_out;
    }

    diagnostics::write(diagnostics::level::info, diagnostics::log_module::motor,
                       "MOTOR_FINISHED", "电机 " + std::to_string(channel) + " 运行完成");
    return motor_run_result::completed;
}

inline motor_run_result motor_run(int channel, bool forward,
                                  motor_interlock::owner owner = motor_interlock::owner::manual) {
    if (!valid_channel(channel))
        return motor_run_result::invalid_channel;
    const int duration_ms = forward
        ? config::motors[channel - 1].load_time.get_value()
        : config::motors[channel - 1].uload_time.get_value();
    return motor_run_controlled(channel, forward, std::chrono::milliseconds(duration_ms), owner);
}





//@brief 发布消息到MQTT服务器
int publish(esp_mqtt_client_handle_t client, const std::string& msg) {
    if (client == nullptr) {
        diagnostics::write(diagnostics::level::error, diagnostics::log_module::mqtt,
                           "MQTT_PUBLISH_FAILED", "MQTT 客户端不可用");
        return -1;
    }
    esp::gpio_out(config::LED_L, true);
    int msg_id = esp_mqtt_client_publish(client, config::topic_publish().c_str(), msg.c_str(), msg.size(), 0, 0);
    if (msg_id < 0) {
        diagnostics::write(diagnostics::level::error, diagnostics::log_module::mqtt,
                           "MQTT_PUBLISH_FAILED", "MQTT 指令提交失败");
    } else {
        diagnostics::write(diagnostics::level::debug, diagnostics::log_module::mqtt,
                           "MQTT_PUBLISH_QUEUED",
                           "MQTT 指令已提交，长度 " + std::to_string(msg.size()) +
                           " 字节，消息 ID " + std::to_string(msg_id));
    }
    esp::gpio_out(config::LED_L, false);
    return msg_id;
}

inline bool wait_for_command_ack(uint32_t sequence, uint32_t previous_version,
                                 std::chrono::milliseconds timeout = 5000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (command_acks.observed_after(sequence, previous_version))
            return true;
        if (flow_should_abort())
            return false;
        mstd::delay(20ms);
    }
    return false;
}

inline bool publish_with_ack(esp_mqtt_client_handle_t client, const std::string& payload,
                             uint32_t sequence,
                             std::chrono::milliseconds timeout = 5000ms) {
    const uint32_t previous_version = command_acks.version();
    if (publish(client, payload) < 0)
        return false;
    if (!wait_for_command_ack(sequence, previous_version, timeout)) {
        diagnostics::write(diagnostics::level::error, diagnostics::log_module::mqtt,
                           "COMMAND_ACK_TIMEOUT",
                           "等待打印机回执超时，sequence_id=" + std::to_string(sequence));
        return false;
    }
    diagnostics::write(diagnostics::level::debug, diagnostics::log_module::mqtt,
                       "COMMAND_ACK_RECEIVED",
                       "打印机已接收命令，sequence_id=" + std::to_string(sequence));
    return true;
}

inline bool send_gcode_with_ack(esp_mqtt_client_handle_t client, const std::string& gcode,
                                std::chrono::milliseconds timeout = 5000ms) {
    const uint32_t sequence = command_sequences.next();
    return publish_with_ack(client, bambu::msg::runGcode(gcode, sequence), sequence, timeout);
}

inline bool send_print_command_with_ack(esp_mqtt_client_handle_t client,
                                        const std::string& command,
                                        std::chrono::milliseconds timeout = 5000ms) {
    const uint32_t sequence = command_sequences.next();
    return publish_with_ack(client, bambu::msg::serialize_print_command(command, sequence),
                            sequence, timeout);
}

inline bool request_status_with_ack(esp_mqtt_client_handle_t client,
                                    std::chrono::milliseconds timeout = 5000ms) {
    const uint32_t sequence = command_sequences.next();
    return publish_with_ack(client, bambu::msg::get_status_with_sequence(sequence),
                            sequence, timeout);
}




// 等待本次命令之后的新 AMS 状态，旧缓存不能满足条件。
inline bool wait_for_fresh_ams_status(int expected, uint32_t previous_version,
                                      std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flow_should_abort())
            return false;
        if (printer_protocol::fresh_state_transition_matches(
                ams_status.load(), expected,
                ams_status_transition_version.load(), previous_version))
            return true;
        mstd::delay(50ms);
    }
    return false;
}

inline bool wait_for_retract_confirmation(uint32_t previous_ams_version,
                                          std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int empty_samples = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flow_should_abort())
            return false;
        if (hw_switch.load() == 0)
            ++empty_samples;
        else
            empty_samples = 0;

        const bool new_idle = printer_protocol::fresh_state_transition_matches(
            ams_status.load(), 0, ams_status_transition_version.load(),
            previous_ams_version);
        if (empty_samples >= 3 && new_idle)
            return true;
        mstd::delay(50ms);
    }
    return false;
}

inline bool wait_for_nozzle_temperature(int target_c,
                                        std::chrono::milliseconds timeout = 300000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    printer_protocol::consecutive_sample_gate ready_samples(
        2, nozzle_actual_version.load());
    while (std::chrono::steady_clock::now() < deadline) {
        if (flow_should_abort())
            return false;
        if (ready_samples.update(
                nozzle_actual_version.load(),
                nozzle_actual_temper.load() >= static_cast<float>(target_c - 5)))
            return true;
        mstd::delay(100ms);
    }
    return false;
}

inline bool wait_for_sensor_state(int expected, std::chrono::milliseconds timeout,
                                  int required_samples = 3) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int stable_samples = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flow_should_abort())
            return false;
        if (hw_switch.load() == expected)
            ++stable_samples;
        else
            stable_samples = 0;
        if (stable_samples >= required_samples)
            return true;
        mstd::delay(50ms);
    }
    return false;
}

inline bool fail_filament_flow(SystemStateGuard& guard, bool automatic,
                               diagnostics::filament_stage stage,
                               std::string_view code, const std::string& detail,
                               bool timeout = false) {
    flow_abort_requested.store(true);
    emergency_stop_motors(detail);
    if (automatic)
        diagnostics::filament_history.fail(stage, detail, timeout);
    filament_log(diagnostics::level::error, code, detail +
                 "；电机已停止，打印保持暂停");
    guard.fail();
    send_printer_operation_status();
    return false;
}

inline bool prepare_filament_flow(SystemStateGuard& guard, bool automatic) {
    const auto fault = printer_faults.snapshot();
    if (!printer_status_ready.load())
        return fail_filament_flow(guard, automatic, diagnostics::filament_stage::trigger,
                                  "PRINTER_STATUS_NOT_READY",
                                  "尚未收到打印机耗材传感器状态");
    if (mqtt_is_unsafe())
        return fail_filament_flow(guard, automatic, diagnostics::filament_stage::trigger,
                                  "MQTT_UNSAFE", "MQTT 未连接或状态已陈旧");
    if (fault.active && fault.interlock)
        return fail_filament_flow(guard, automatic, diagnostics::filament_stage::trigger,
                                  "PRINTER_FAULT_ACTIVE",
                                  "打印机故障尚未清除：" + fault.display_code +
                                  " " + fault.label_zh);

    flow_abort_requested.store(false);
    motor_arbiter.clear_stop_request();
    flow_phase = "trigger";
    return true;
}

inline bool finish_and_resume(esp_mqtt_client_handle_t client,
                              SystemStateGuard& guard, bool automatic,
                              int target_channel,
                              diagnostics::run_state final_state =
                                  diagnostics::run_state::success) {
    if (!automatic)
        return true;

    const auto fault = printer_faults.snapshot();
    const filament_flow_policy::resume_evidence evidence{
        .printer_status_ready = printer_status_ready.load(),
        .mqtt_fresh = !mqtt_is_unsafe(),
        .fault_free = !fault.active || !fault.interlock,
        .sensor_present = hw_switch.load() == 1,
        .channel_confirmed = extruder_confirmed.get_value(),
        .current_channel = extruder.get_value(),
        .target_channel = target_channel,
    };
    if (!filament_flow_policy::can_resume(evidence)) {
        return fail_filament_flow(guard, true, diagnostics::filament_stage::resume,
                                  "RESUME_EVIDENCE_MISSING",
                                  "恢复前的传感器、通道确认或链路证据不完整");
    }

    flow_phase = "resuming";
    diagnostics::filament_history.start(diagnostics::filament_stage::resume,
                                         "恢复热床目标并提交恢复命令");
    const int bed_restore_target = bed_target_temper_max.load();
    if (bed_restore_target > 0 &&
        !send_gcode_with_ack(client, "M190 S" + std::to_string(bed_restore_target))) {
        return fail_filament_flow(guard, true, diagnostics::filament_stage::resume,
                                  "BED_RESTORE_ACK_TIMEOUT",
                                  "恢复热床目标命令未收到同序列回执");
    }
    if (!send_print_command_with_ack(client, "resume")) {
        return fail_filament_flow(guard, true, diagnostics::filament_stage::resume,
                                  "RESUME_ACK_TIMEOUT",
                                  "恢复打印命令未收到同序列回执");
    }
    diagnostics::filament_history.finish_stage(diagnostics::filament_stage::resume,
                                                diagnostics::stage_state::success,
                                                "打印机已接收恢复命令");
    diagnostics::filament_history.finish_run(final_state);
    return true;
}

void run_filament_flow(esp_mqtt_client_handle_t client, int new_extruder,
                       bool automatic, bool initial_load) {
    SystemStateGuard state_guard;
    operation_status = automatic ? "changing" : "loading";

    const int old_extruder = extruder.get_value();
    filament_log(diagnostics::level::info,
                 initial_load ? "INITIAL_LOAD_STARTED" :
                 (automatic ? "CHANGE_STARTED" : "LOAD_STARTED"),
                 std::string(initial_load ? "开始首料握手: " :
                             (automatic ? "开始自动换料: " : "开始手动上料: ")) +
                 "通道 " + std::to_string(old_extruder) + " → " +
                 std::to_string(new_extruder));

    if (!valid_channel(new_extruder)) {
        fail_filament_flow(state_guard, automatic, diagnostics::filament_stage::trigger,
                           "FLOW_INVALID_CHANNEL",
                           "目标通道无效：" + std::to_string(new_extruder));
        return;
    }
    if (!prepare_filament_flow(state_guard, automatic))
        return;

    const int initial_sensor = hw_switch.load();
    const bool channel_known = extruder_confirmed.get_value() &&
                               valid_channel(old_extruder);

    if (initial_sensor == 1 && !channel_known) {
        fail_filament_flow(state_guard, automatic, diagnostics::filament_stage::unload,
                           "CHANNEL_CONFIRMATION_REQUIRED",
                           "打印机检测到有料，但真实通道尚未确认");
        return;
    }
    if (initial_sensor < 0) {
        fail_filament_flow(state_guard, automatic, diagnostics::filament_stage::trigger,
                           "FILAMENT_SENSOR_UNKNOWN",
                           "打印机耗材传感器状态未知");
        return;
    }

    if (initial_sensor == 1 && old_extruder == new_extruder) {
        if (automatic) {
            diagnostics::filament_history.skip(diagnostics::filament_stage::unload,
                                                "已装载目标通道");
            diagnostics::filament_history.skip(diagnostics::filament_stage::retract,
                                                "已装载目标通道");
            diagnostics::filament_history.skip(diagnostics::filament_stage::heat,
                                                "已装载目标通道");
            diagnostics::filament_history.skip(diagnostics::filament_stage::feed,
                                                "已装载目标通道");
        }
        set_current_channel(new_extruder, true);
        if (!finish_and_resume(client, state_guard, automatic, new_extruder,
                               diagnostics::run_state::skipped))
            return;
        filament_log(diagnostics::level::info, "FLOW_ALREADY_LOADED",
                     "目标通道已装载并确认，无需驱动电机");
        state_guard.release();
        send_printer_operation_status();
        return;
    }

    if (initial_sensor == 1) {
        const int unload_time_ms =
            config::motors[old_extruder - 1].uload_time.get_value();
        if (unload_time_ms <= 0) {
            fail_filament_flow(state_guard, automatic, diagnostics::filament_stage::unload,
                               "UNLOAD_TIME_INVALID",
                               "旧通道未配置有效的完整退线时间");
            return;
        }

        flow_phase = "unloading";
        if (automatic)
            diagnostics::filament_history.start(diagnostics::filament_stage::unload,
                                                 "发送退料命令并等待新鲜 AMS 260");
        const uint32_t unload_status_version = ams_status_version.load();
        const std::string unload_gcode =
            "M109 S" + std::to_string(config::motors[old_extruder - 1].temper.get_value()) +
            "\nM620 S255\nT255\nM621 S255";
        if (!send_gcode_with_ack(client, unload_gcode)) {
            fail_filament_flow(state_guard, automatic, diagnostics::filament_stage::unload,
                               "UNLOAD_ACK_TIMEOUT",
                               "退料 G-code 未收到同序列回执");
            return;
        }
        if (!wait_for_fresh_ams_status(0x0104, unload_status_version, 120000ms)) {
            fail_filament_flow(state_guard, automatic, diagnostics::filament_stage::unload,
                               "UNLOAD_STATUS_TIMEOUT",
                               "等待本次退料进入 AMS 260 超时", true);
            return;
        }
        if (automatic)
            diagnostics::filament_history.finish_stage(
                diagnostics::filament_stage::unload,
                diagnostics::stage_state::success,
                "收到本次流程的新鲜 AMS 260");

        flow_phase = "retracting";
        if (automatic)
            diagnostics::filament_history.start(diagnostics::filament_stage::retract,
                                                 "旧通道完整退线并确认无料/空闲");
        // AMS 空闲可能在完整退线期间甚至刚进入退线时到达。沿用发送退料
        // 命令前的基线，既接受属于本次命令的空闲转换，也拒绝旧缓存。
        const uint32_t retract_status_version = unload_status_version;
        const auto retract_result = motor_run_controlled(
            old_extruder, false, std::chrono::milliseconds(unload_time_ms),
            motor_interlock::owner::unload);
        if (retract_result != motor_run_result::completed) {
            fail_filament_flow(state_guard, automatic, diagnostics::filament_stage::retract,
                               "RETRACT_MOTOR_FAILED",
                               "旧通道完整退线未完成");
            return;
        }
        if (!wait_for_retract_confirmation(retract_status_version, 30000ms)) {
            fail_filament_flow(state_guard, automatic, diagnostics::filament_stage::retract,
                               "RETRACT_CONFIRM_TIMEOUT",
                               "退线后未同时确认稳定无料和新的 AMS 空闲状态", true);
            return;
        }
        set_current_channel(filament_flow_policy::channel_after_attempt(
                                old_extruder, new_extruder, true, false),
                            false);
        if (automatic)
            diagnostics::filament_history.finish_stage(
                diagnostics::filament_stage::retract,
                diagnostics::stage_state::success,
                "退线完成，传感器稳定无料且 AMS 空闲");
    } else {
        set_current_channel(0, false);
        if (automatic) {
            diagnostics::filament_history.skip(diagnostics::filament_stage::unload,
                                                "打印机已明确报告无料");
            diagnostics::filament_history.skip(diagnostics::filament_stage::retract,
                                                "没有旧通道料线");
        }
    }

    const int load_time_ms = config::motors[new_extruder - 1].load_time.get_value();
    if (load_time_ms <= 0) {
        fail_filament_flow(state_guard, automatic, diagnostics::filament_stage::feed,
                           "LOAD_TIME_INVALID",
                           "目标通道最大进料时间必须大于零");
        return;
    }

    const int target_temperature =
        config::motors[new_extruder - 1].temper.get_value();
    flow_phase = "heating";
    if (automatic)
        diagnostics::filament_history.start(
            diagnostics::filament_stage::heat,
            "等待实际喷嘴温度达到 " + std::to_string(target_temperature - 5) + "°C");
    if (!send_gcode_with_ack(client, "M109 S" + std::to_string(target_temperature))) {
        fail_filament_flow(state_guard, automatic, diagnostics::filament_stage::heat,
                           "HEAT_ACK_TIMEOUT",
                           "加热 G-code 未收到同序列回执");
        return;
    }
    if (!wait_for_nozzle_temperature(target_temperature)) {
        fail_filament_flow(state_guard, automatic, diagnostics::filament_stage::heat,
                           "HEAT_TIMEOUT",
                           "实际喷嘴温度未在 300 秒内连续两次达到阈值", true);
        return;
    }
    if (automatic)
        diagnostics::filament_history.finish_stage(
            diagnostics::filament_stage::heat,
            diagnostics::stage_state::success,
            "实际喷嘴温度连续两次达到进料条件");

    if (!wait_for_sensor_state(0, 2000ms)) {
        fail_filament_flow(state_guard, automatic, diagnostics::filament_stage::feed,
                           "FEED_START_NOT_EMPTY",
                           "进料前耗材传感器未稳定处于无料");
        return;
    }

    pending_feed_channel.store(new_extruder);
    flow_phase = "feed_command";
    if (automatic)
        diagnostics::filament_history.start(diagnostics::filament_stage::feed,
                                             "目标通道进线，等待无料到有料转换");
    if (!send_gcode_with_ack(client, "G1 E150 F500")) {
        fail_filament_flow(state_guard, automatic, diagnostics::filament_stage::feed,
                           "FEED_ACK_TIMEOUT",
                           "挤出机辅助进料 G-code 未收到同序列回执");
        return;
    }

    flow_phase = "feeding";
    int present_samples = 0;
    const auto sensor_present = [&present_samples]() {
        if (hw_switch.load() == 1)
            ++present_samples;
        else
            present_samples = 0;
        return present_samples >= 5; // 20 ms × 5 = 100 ms 稳定确认
    };
    const auto feed_result = motor_run_controlled(
        new_extruder, true, std::chrono::milliseconds(load_time_ms),
        motor_interlock::owner::load, sensor_present, true, 500ms);
    if (feed_result != motor_run_result::completed) {
        fail_filament_flow(state_guard, automatic, diagnostics::filament_stage::feed,
                           "FEED_SENSOR_TIMEOUT",
                           "目标通道在最大进料时间内未形成稳定无料到有料转换", true);
        return;
    }

    set_current_channel(filament_flow_policy::channel_after_attempt(
                            old_extruder, new_extruder, true, true),
                        true);
    if (automatic)
        diagnostics::filament_history.finish_stage(
            diagnostics::filament_stage::feed,
            diagnostics::stage_state::success,
            "传感器确认有料，完成 500 ms 交接");
    pending_feed_channel.store(0);

    if (!finish_and_resume(client, state_guard, automatic, new_extruder))
        return;

    filament_log(diagnostics::level::info,
                 automatic ? "CHANGE_FINISHED" : "LOAD_FINISHED",
                 std::string(automatic ? "闭环换料完成: 通道 " :
                                        "闭环手动上料完成: 通道 ") +
                 std::to_string(new_extruder));
    state_guard.release();
    send_printer_operation_status();
}

esp_mqtt_client_handle_t __client = nullptr;

void load_filament(int new_extruder) {
    if (__client == nullptr) {
        filament_log(diagnostics::level::error, "LOAD_MQTT_UNAVAILABLE",
                     "MQTT 客户端未初始化，无法执行上料");
        pause_lock = false;
        return;
    }
    run_filament_flow(__client, new_extruder, false, false);
}

void work(mesp::Mqttclient& Mqtt) {//之后应该修改好mesp::Mqttclient生命周期@_@
    __client = Mqtt.client;
    Mqtt.subscribe(config::topic_subscribe());// 订阅消息

    printer_status_ready.store(false);
    hw_switch.store(-1);
    flow_abort_requested.store(false);
    motor_arbiter.clear_stop_request();
    filament_log(diagnostics::level::info, "FILAMENT_PROBE",
                 "请求打印机真实耗材状态和通道确认状态");

    const bool request_accepted = request_status_with_ack(__client);
    const auto status_deadline = std::chrono::steady_clock::now() + 5000ms;
    while (request_accepted && !printer_status_ready.load() &&
           std::chrono::steady_clock::now() < status_deadline) {
        mstd::delay(50ms);
    }

    if (!request_accepted || !printer_status_ready.load()) {
        set_current_channel(0, false);
        filament_log(diagnostics::level::error, "FILAMENT_PROBE_TIMEOUT",
                     "未取得首份真实耗材状态；辅助送料和自动流程保持禁用");
    } else if (hw_switch.load() == 0) {
        set_current_channel(0, false);
        filament_log(diagnostics::level::info, "FILAMENT_EMPTY",
                     "打印机明确报告无料，当前通道设置为 0");
    } else if (hw_switch.load() == 1) {
        const int saved_channel = extruder.get_value();
        if (extruder_confirmed.get_value() && valid_channel(saved_channel)) {
            filament_log(diagnostics::level::info, "FILAMENT_DETECTED",
                         "检测到有料，已确认当前通道 " + std::to_string(saved_channel));
        } else {
            set_current_channel(0, false);
            filament_log(diagnostics::level::warning, "FILAMENT_CHANNEL_REQUIRED",
                         "检测到有料但真实通道未确认，请在页面选择当前通道");
        }
    }
    printer_status_updated_ms.store(diagnostics::uptime_ms());
    send_printer_operation_status();

    int cnt = 0;
    while (true) {
        mstd::delay(20000ms);
        // esp::gpio_out(esp::LED_R, cnt % 2);
        ++cnt;
    }
}

inline void observe_command_ack(JsonVariantConst group) {
    if (group.isNull() || group["sequence_id"].isNull())
        return;

    uint32_t parsed = 0;
    if (group["sequence_id"].is<const char*>()) {
        const char* value = group["sequence_id"].as<const char*>();
        if (value == nullptr || !printer_protocol::parse_sequence_id(value, parsed))
            return;
    } else if (group["sequence_id"].is<uint32_t>()) {
        parsed = group["sequence_id"].as<uint32_t>();
    } else {
        return;
    }
    command_acks.observe(parsed);
}

inline printer_status::fault_info decode_hms_array(JsonVariantConst hms) {
    printer_status::fault_info selected;
    if (!hms.is<JsonArrayConst>())
        return selected;

    for (JsonObjectConst item : hms.as<JsonArrayConst>()) {
        if (item["attr"].isNull() || item["code"].isNull())
            continue;
        const auto decoded = printer_status::decode_hms(
            item["attr"].as<uint32_t>(), item["code"].as<uint32_t>());
        if (!selected.active ||
            static_cast<int>(decoded.severity) > static_cast<int>(selected.severity) ||
            (decoded.severity == selected.severity && decoded.known && !selected.known)) {
            selected = decoded;
        }
    }
    return selected;
}

//@brief MQTT回调函数
void callback_fun(esp_mqtt_client_handle_t client, const std::string& json) {
    using namespace ArduinoJson;
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, json);
    if (error) {
        diagnostics::write(diagnostics::level::warning, diagnostics::log_module::mqtt,
                           "MQTT_JSON_INVALID", "收到无法解析的 MQTT JSON");
        return;
    }

    JsonVariantConst print = doc["print"];
    observe_command_ack(print);
    observe_command_ack(doc["pushing"]);

    bool operation_changed = false;
    bool operation_touched = false;

    if (!print["bed_target_temper"].isNull())
        change_trigger.update_bed_target(print["bed_target_temper"].as<int>());
    if (!print["gcode_state"].isNull())
        change_trigger.update_gcode_state(print["gcode_state"].as<std::string>());
    if (!print["nozzle_target_temper"].isNull())
        nozzle_target_temper.store(print["nozzle_target_temper"].as<int>());
    if (!print["nozzle_temper"].isNull()) {
        nozzle_actual_temper.store(print["nozzle_temper"].as<float>());
        nozzle_actual_version.fetch_add(1);
        operation_touched = true;
    }

    if (!print["hw_switch_state"].isNull()) {
        const int raw_sensor = print["hw_switch_state"].as<int>();
        const int sensor = raw_sensor == 0 ? 0 : raw_sensor == 1 ? 1 : -1;
        const int previous_sensor = hw_switch.exchange(sensor);
        const bool ready = sensor != -1;
        const bool was_ready = printer_status_ready.exchange(ready);
        const bool sensor_changed = previous_sensor != sensor || was_ready != ready;
        operation_changed = operation_changed || sensor_changed;
        operation_touched = true;

        if (!ready && sensor_changed) {
            diagnostics::write(diagnostics::level::warning,
                               diagnostics::log_module::filament,
                               "FILAMENT_SENSOR_VALUE_UNKNOWN",
                               "打印机返回未识别的 hw_switch_state=" +
                                   std::to_string(raw_sensor));
            if (system_locked.get_value()) {
                flow_abort_requested.store(true);
                emergency_stop_motors("打印机耗材传感器状态变为未知");
                flow_phase = "fault";
            }
        }

        if (sensor == 0 && !system_locked.get_value() &&
            motor_arbiter.current() == motor_interlock::owner::none) {
            if (extruder.get_value() != 0 || extruder_confirmed.get_value()) {
                set_current_channel(0, false);
                operation_changed = true;
            }
        }
    }

    if (!print["ams_status"].isNull()) {
        const int current = print["ams_status"].as<int>();
        const int previous = ams_status.exchange(current);
        const uint32_t current_version = ams_status_version.fetch_add(1) + 1;
        if (previous != current)
            ams_status_transition_version.store(current_version);
        ams_status.notify_all();
        operation_changed = operation_changed || previous != current;
        operation_touched = true;
        if (previous != current) {
            const auto decoded = printer_status::decode_ams_status(current);
            diagnostics::write(diagnostics::level::debug, diagnostics::log_module::filament,
                               "AMS_STATUS",
                               "AMS " + std::to_string(current) + "（" +
                               decoded.label_zh + "）");
        }
    }

    if (!print["print_error"].isNull()) {
        const uint32_t current_error = print["print_error"].as<uint32_t>();
        print_error.store(current_error);
        operation_changed =
            printer_faults.update_print_error(current_error) || operation_changed;
        operation_touched = true;
    }
    if (!print["hms"].isNull()) {
        operation_changed =
            printer_faults.update_hms(decode_hms_array(print["hms"])) ||
            operation_changed;
        operation_touched = true;
    }

    const auto fault = printer_faults.snapshot();
    if (operation_changed && fault.active && fault.interlock) {
        flow_abort_requested.store(true);
        emergency_stop_motors("打印机故障 " + fault.display_code + "：" + fault.label_zh);
        if (system_locked.get_value())
            flow_phase = "fault";
        diagnostics::write(diagnostics::level::error, diagnostics::log_module::filament,
                           "PRINTER_INTERLOCK",
                           fault.display_code + " " + fault.label_zh +
                           "；电机已停止，打印保持暂停");
    }

    if (operation_touched)
        printer_status_updated_ms.store(diagnostics::uptime_ms());
    if (operation_changed)
        send_printer_operation_status();

    const int bed_target_temper = change_trigger.bed_target();
    if (bed_target_temper == 0) {
        bed_target_temper_max.store(0);
    } else if (!(bed_target_temper > 0 && bed_target_temper < 17)) {
        int observed = bed_target_temper_max.load();
        while (bed_target_temper > observed &&
               !bed_target_temper_max.compare_exchange_weak(observed, bed_target_temper)) {
        }
    }

    const auto readiness = change_trigger.evaluate(config::active_motor_count);
    const auto request = change_trigger.request(config::active_motor_count);
    if (readiness != last_change_trigger_readiness) {
        if (readiness == filament_change::trigger_readiness::waiting_for_pause) {
            diagnostics::write(diagnostics::level::debug, diagnostics::log_module::filament,
                               "CHANGE_WAITING_FOR_PAUSE",
                               "已收到通道标记，等待打印机进入 PAUSE");
        } else if (readiness == filament_change::trigger_readiness::waiting_for_channel) {
            diagnostics::write(diagnostics::level::debug, diagnostics::log_module::filament,
                               "CHANGE_WAITING_FOR_CHANNEL",
                               "已收到 PAUSE，等待有效通道标记");
        }
        last_change_trigger_readiness = readiness;
        if (readiness != filament_change::trigger_readiness::ready)
            change_trigger_busy_logged = false;
    }

    if (readiness == filament_change::trigger_readiness::invalid_channel) {
        const int raw_marker = bed_target_temper;
        change_trigger.consume();
        last_change_trigger_readiness = filament_change::trigger_readiness::consumed;
        diagnostics::filament_history.begin(extruder.get_value(), raw_marker,
                                             "检测到无效暂停通道标记");
        diagnostics::filament_history.finish_stage(
            diagnostics::filament_stage::trigger,
            diagnostics::stage_state::error,
            "目标通道标记无效");
        diagnostics::filament_history.skip(diagnostics::filament_stage::unload, "触发失败");
        diagnostics::filament_history.skip(diagnostics::filament_stage::retract, "触发失败");
        diagnostics::filament_history.skip(diagnostics::filament_stage::heat, "触发失败");
        diagnostics::filament_history.skip(diagnostics::filament_stage::feed, "触发失败");
        diagnostics::filament_history.skip(diagnostics::filament_stage::resume,
                                            "保持打印暂停");
        diagnostics::filament_history.finish_run(diagnostics::run_state::error);
        flow_phase = "fault";
        diagnostics::write(diagnostics::level::error, diagnostics::log_module::filament,
                           "CHANGE_INVALID_CHANNEL",
                           "无效通道标记 " + std::to_string(raw_marker) +
                           "；不恢复打印");
        send_printer_operation_status();
        return;
    }

    if (readiness != filament_change::trigger_readiness::ready)
        return;

    if (flow_phase.get_value() == "fault") {
        change_trigger.consume();
        last_change_trigger_readiness = filament_change::trigger_readiness::consumed;
        diagnostics::write(
            diagnostics::level::warning, diagnostics::log_module::filament,
            "CHANGE_RETRY_REQUIRES_USER",
            "上次流程已故障；忽略本次缓存触发，等待用户排障并手动重试");
        send_printer_operation_status();
        return;
    }

    if (system_locked.get_value() || pause_lock.load()) {
        if (!change_trigger_busy_logged) {
            diagnostics::write(diagnostics::level::warning, diagnostics::log_module::filament,
                               "CHANGE_BUSY", "系统忙碌，保留当前换料请求");
            change_trigger_busy_logged = true;
        }
        return;
    }

    bool expected = false;
    if (!pause_lock.compare_exchange_strong(expected, true)) {
        if (!change_trigger_busy_logged) {
            diagnostics::write(diagnostics::level::warning, diagnostics::log_module::filament,
                               "CHANGE_BUSY", "换料锁已被占用，保留当前请求");
            change_trigger_busy_logged = true;
        }
        return;
    }

    const int old_extruder = extruder.get_value();
    const bool initial_load =
        request.kind == filament_change::trigger_kind::initial_load;
    diagnostics::filament_history.begin(
        old_extruder, request.channel,
        initial_load ? "确认 PAUSE 与自动首料标记" :
                       "确认 PAUSE 与普通换料标记");
    diagnostics::filament_history.finish_stage(
        diagnostics::filament_stage::trigger,
        diagnostics::stage_state::success,
        initial_load ? "自动首料触发已确认" : "换料触发已确认");
    diagnostics::write(diagnostics::level::info, diagnostics::log_module::filament,
                       initial_load ? "INITIAL_LOAD_TRIGGER_ACCEPTED" :
                                      "CHANGE_TRIGGER_ACCEPTED",
                       std::string(initial_load ? "接受自动首料请求: " :
                                                  "接受自动换料请求: ") +
                       "通道 " + std::to_string(old_extruder) + " → " +
                       std::to_string(request.channel));

    async_channel.emplace([=]() {
        run_filament_flow(client, request.channel, true, initial_load);
    });
    change_trigger.consume();
    last_change_trigger_readiness = filament_change::trigger_readiness::consumed;
    change_trigger_busy_logged = false;
    diagnostics::write(diagnostics::level::info, diagnostics::log_module::filament,
                       "CHANGE_QUEUED",
                       "闭环耗材流程已入队，目标通道 " +
                       std::to_string(request.channel));
}// callback

inline bool reserve_manual_request(int channel) {
    if (!valid_channel(channel) || system_locked.get_value() || pause_lock.load() ||
        motor_arbiter.current() != motor_interlock::owner::none ||
        !printer_status_ready.load() || mqtt_is_unsafe())
        return false;
    const auto fault = printer_faults.snapshot();
    if (fault.active && fault.interlock)
        return false;
    bool expected = false;
    return pause_lock.compare_exchange_strong(expected, true);
}

inline void run_manual_motor(int channel, bool forward) {
    SystemStateGuard guard;
    operation_status = "loading";
    if (!prepare_filament_flow(guard, false))
        return;
    flow_phase = "manual";
    const auto result = motor_run(channel, forward, motor_interlock::owner::manual);
    if (result != motor_run_result::completed) {
        fail_filament_flow(guard, false, diagnostics::filament_stage::feed,
                           "MANUAL_MOTOR_FAILED", "手动电机动作未完成");
        return;
    }
    guard.release();
    send_printer_operation_status();
}

void Task1(void* param) {
    esp::gpio_set_in(config::forward_click);
    motor_interlock::falling_edge_debouncer debounce(5);

    while (true) {
        const bool pressed = gpio_get_level(config::forward_click) == 0;
        if (!debounce.update(pressed)) {
            mstd::delay(20ms);
            continue;
        }

        if (config::assist_feeding_enabled.get_value() != 1) {
            diagnostics::write(diagnostics::level::debug, diagnostics::log_module::motor,
                               "ASSIST_DISABLED", "微动稳定按下，但辅助进料已关闭");
            mstd::delay(20ms);
            continue;
        }

        const auto fault = printer_faults.snapshot();
        const std::string phase = flow_phase.get_value();
        int channel = 0;
        if (phase == "feeding") {
            channel = pending_feed_channel.load();
        } else if (phase == "idle" && !system_locked.get_value() &&
                   !pause_lock.load() &&
                   printer_status_ready.load() && hw_switch.load() == 1 &&
                   extruder_confirmed.get_value()) {
            channel = extruder.get_value();
        }

        if (!valid_channel(channel) || !printer_status_ready.load() ||
            mqtt_is_unsafe() || (fault.active && fault.interlock) ||
            (phase != "idle" && phase != "feeding")) {
            diagnostics::write(diagnostics::level::warning, diagnostics::log_module::motor,
                               "ASSIST_INTERLOCKED",
                               "微动触发被安全联锁拒绝，阶段=" + phase);
            mstd::delay(20ms);
            continue;
        }

        // 自动进料阶段已经持续驱动同一个目标通道。微动按下只确认通道
        // 一致性，不另行争抢 owner，避免在主进料取得互斥锁前插入 1 秒脉冲。
        if (phase == "feeding") {
            diagnostics::write(
                diagnostics::level::debug, diagnostics::log_module::motor,
                "ASSIST_TARGET_ALREADY_FEEDING",
                "微动目标通道 " + std::to_string(channel) +
                    " 已由自动进料持续驱动，不重复启动电机");
            mstd::delay(20ms);
            continue;
        }

        if (phase == "idle") {
            flow_abort_requested.store(false);
            motor_arbiter.clear_stop_request();
        }
        diagnostics::write(diagnostics::level::info, diagnostics::log_module::motor,
                           "ASSIST_TRIGGERED",
                           "微动稳定按下，通道 " + std::to_string(channel) +
                           " 执行一次 1 秒辅助脉冲");
        motor_run_controlled(channel, true, 1000ms, motor_interlock::owner::assist);
        mstd::delay(20ms);
    }
}//微动缓冲程序


//延时检测
void Task2(void* param) {
    while (true) {
        mstd::delay(1000ms);
        if (diagnostics::mqtt_metrics.mark_stale_if_needed()) {
            diagnostics::write(diagnostics::level::warning, diagnostics::log_module::mqtt,
                               "MQTT_DATA_STALE", "MQTT 已超过 8 秒未收到打印机数据");
            printer_status_ready.store(false);
            flow_abort_requested.store(true);
            emergency_stop_motors("MQTT 超过 8 秒未收到打印机数据");
            flow_phase = "fault";
            printer_status_updated_ms.store(diagnostics::uptime_ms());
            send_printer_operation_status();
        }
    }
}

#include "index.hpp"

volatile bool running_flag{false};

extern "C" void app_main() {
    diagnostics::write(diagnostics::level::info, diagnostics::log_module::system,
                       "SYSTEM_BOOT", "Top-AMS 固件启动");
#ifndef LOCAL_CONFIG
    for (size_t i = 0; i < config::active_motor_count; i++) {
        auto& x = config::motors[i];
        esp::gpio_out(x.forward, false);
        esp::gpio_out(x.backward, false);
    }//初始化电机GPIO
#endif

    // 旧版本曾把检测到的耗材默认归为通道 1。升级后只有持久化且已确认的
    // 合法通道可以保留；其余情况在首次真实传感器状态到达前显示为未确认。
    const int startup_channel = filament_flow_policy::confirmed_startup_channel(
        extruder.get_value(), extruder_confirmed.get_value(),
        config::active_motor_count);
    if (startup_channel == 0)
        set_current_channel(0, false);

    xTaskCreate(Task1, "Task1", 2048, NULL, 1, &Task1_handle);//微动任务
    xTaskCreate(Task2, "Task2", 2048, NULL, 1, &Task2_handle);//延时调试信息


    {// wifi连接部分
        mesp::ConfigStore wificonfig("wificonfig");

        string Wifi_ssid = wificonfig.get("Wifi_ssid", "");
        string Wifi_pass = wificonfig.get("Wifi_pass", "");

        if (Wifi_ssid == "") {
            diagnostics::write(diagnostics::level::info, diagnostics::log_module::wifi,
                               "WIFI_SMARTCONFIG", "等待 SmartConfig 配网");
            WiFi.mode(WIFI_AP_STA);
            WiFi.beginSmartConfig();

            int cnt = 0;
            while (!WiFi.smartConfigDone()) {
                delay(1000);
                esp::gpio_out(config::LED_R, cnt % 2);
                ++cnt;
            }

            Wifi_ssid = WiFi.SSID().c_str();
            Wifi_pass = WiFi.psk().c_str();

            wificonfig.set("Wifi_ssid", Wifi_ssid);
            wificonfig.set("Wifi_pass", Wifi_pass);
        } else {
            diagnostics::write(diagnostics::level::info, diagnostics::log_module::wifi,
                               "WIFI_CONNECTING", "正在连接已保存的 Wi-Fi");
            WiFi.begin(Wifi_ssid.c_str(), Wifi_pass.c_str());
        }

        // 等待WiFi连接到路由器
        while (WiFi.status() != WL_CONNECTED) {
            delay(500);
        }

        diagnostics::write(diagnostics::level::info, diagnostics::log_module::wifi,
                           "WIFI_CONNECTED",
                           std::string("Wi-Fi 已连接，IP ") + WiFi.localIP().toString().c_str());
        esp::gpio_out(config::LED_R, false);
    }// wifi连接部分


    using namespace config;
    using namespace ArduinoJson;
    using std::string;

    //异步任务处理,线程池
    std::thread async_thread([]() {
        while (true) {
            auto task = async_channel.pop();
            task();
        }
    });

    std::binary_semaphore mqtt_Signal{0};

    {//服务器配置部分
        server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
            // The const char* overload copies the whole page into an Arduino String.
            // Use the fixed-length program-memory response to avoid a ~64 KB heap allocation.
            request->send(200, "text/html; charset=utf-8",
                          reinterpret_cast<const uint8_t*>(web.data()), web.size());
        });

        // 配置 WebSocket 事件处理
        ws.onEvent([&mqtt_Signal](AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
            if (type == WS_EVT_CONNECT) {
                send_wifi_status(client);
                send_diagnostics_snapshot(client);
                send_printer_operation_status(client);
                send_filament_timeline(client);
                send_diagnostics_log_history(client);

                JsonDocument doc;
                JsonObject root = doc.to<JsonObject>();
                root.createNestedArray("data");// 创建data数组

                for (auto& [name, to_json] : mesp::ws_value_to_json)
                    to_json(doc);// 添加当前值到data数组
                String state_msg;
                serializeJson(doc, state_msg);
                client->text(state_msg);// 只向新连接的客户端发送初始状态

                diagnostics::write(diagnostics::level::info, diagnostics::log_module::web,
                                   "WS_CONNECTED",
                                   "WebSocket 客户端 " + std::to_string(client->id()) + " 已连接");
            } else if (type == WS_EVT_DISCONNECT) {
                diagnostics::write(diagnostics::level::info, diagnostics::log_module::web,
                                   "WS_DISCONNECTED",
                                   "WebSocket 客户端 " + std::to_string(client->id()) + " 已断开");
            } else if (type == WS_EVT_DATA) {// 处理接收到的数据
                JsonDocument doc;
                const DeserializationError error = deserializeJson(doc, data, len);
                if (error) {
                    diagnostics::write(diagnostics::level::warning, diagnostics::log_module::web,
                                       "WS_JSON_INVALID", "收到无法解析的 WebSocket JSON");
                    return;
                }


                if (doc.containsKey("data") && doc["data"].is<JsonArray>()) {
                    for (JsonObject obj : doc["data"].as<JsonArray>()) {
                        if (obj.containsKey("name")) {
                            std::string name = obj["name"].as<std::string>();

                            if (name == "extruder") {
                                const int requested = obj["value"] | -1;
                                const bool idle = !system_locked.get_value() &&
                                    !pause_lock.load() &&
                                    motor_arbiter.current() == motor_interlock::owner::none;
                                const bool confirm_loaded = idle && printer_status_ready.load() &&
                                    hw_switch.load() == 1 && valid_channel(requested);
                                const bool confirm_empty = idle && printer_status_ready.load() &&
                                    hw_switch.load() == 0 && requested == 0;
                                if (confirm_loaded || confirm_empty) {
                                    set_current_channel(requested, confirm_loaded);
                                    diagnostics::write(
                                        diagnostics::level::info,
                                        diagnostics::log_module::filament,
                                        "CHANNEL_CONFIRMED",
                                        confirm_loaded
                                            ? "用户确认当前通道 " + std::to_string(requested)
                                            : "用户确认打印机当前无料");
                                    send_printer_operation_status();
                                } else {
                                    diagnostics::write(
                                        diagnostics::level::warning,
                                        diagnostics::log_module::filament,
                                        "CHANNEL_CONFIRM_REJECTED",
                                        "通道确认被拒绝：状态未知、无料或系统忙碌");
                                    sync_ws_value("extruder");
                                    sync_ws_value("extruder_confirmed");
                                }
                                continue;
                            }
                            if (name == "extruder_confirmed") {
                                sync_ws_value("extruder_confirmed");
                                continue;
                            }

                            auto it = mesp::ws_value_update.find(name);
                            if (it != mesp::ws_value_update.end()) {
                                const bool is_reverse_setting = is_motor_reverse_setting(name);
                                if (is_reverse_setting && !can_update_motor_direction()) {
                                    diagnostics::write(diagnostics::level::warning, diagnostics::log_module::motor,
                                                       "MOTOR_CONFIG_BUSY", "系统忙碌，无法修改电机输出方向");
                                    sync_ws_value(name);//拒绝修改并恢复前端显示
                                } else {
                                    it->second(obj);//更新值
                                    if (is_reverse_setting)
                                        sync_ws_value(name);//向所有客户端同步持久化后的布尔值
                                }
                            }

                            if (name == "device_serial") {//需要连接mqtt,放这里感觉有些耦合
                                mqtt_Signal.release();
                            }
                        }
                    }
                }//wsvalue更新部分


                const std::string command = doc["action"]["command"] | string("_null");
                if (command != "_null") {//处理命令json
                    if (command == "get_wifi_status") {
                        send_wifi_status(client);
                    } else if (command == "get_diagnostics_snapshot") {
                        send_diagnostics_snapshot(client);
                    } else if (command == "clear_diagnostic_logs") {
                        diagnostics::clear_logs();
                    } else if (command == "motor_forward") {//电机前向控制
                        int motor_id = doc["action"]["value"] | -1;
                        if (reserve_manual_request(motor_id)) {
                            async_channel.emplace([motor_id]() {
                                run_manual_motor(motor_id, true);
                            });
                        } else {
                            diagnostics::write(diagnostics::level::warning,
                                               diagnostics::log_module::motor,
                                               "MANUAL_MOTOR_REJECTED",
                                               "手动正转请求被服务端安全联锁拒绝");
                        }
                    } else if (command == "motor_backward") {//电机前向控制
                        int motor_id = doc["action"]["value"] | -1;
                        if (reserve_manual_request(motor_id)) {
                            async_channel.emplace([motor_id]() {
                                run_manual_motor(motor_id, false);
                            });
                        } else {
                            diagnostics::write(diagnostics::level::warning,
                                               diagnostics::log_module::motor,
                                               "MANUAL_MOTOR_REJECTED",
                                               "手动反转请求被服务端安全联锁拒绝");
                        }
                    } else if (command == "load_filament") {
                        int new_extruder = doc["action"]["value"] | -1;
                        if (reserve_manual_request(new_extruder)) {
                            async_channel.emplace([new_extruder]() {
                                load_filament(new_extruder);
                            });
                        } else {
                            diagnostics::write(diagnostics::level::warning,
                                               diagnostics::log_module::filament,
                                               "LOAD_REQUEST_REJECTED",
                                               "手动上料请求被服务端安全联锁拒绝");
                        }
                    } else {
                        diagnostics::write(diagnostics::level::warning, diagnostics::log_module::web,
                                           "WS_COMMAND_UNKNOWN", "未知 WebSocket 命令: " + command);
                    }
                }//if command

            }//WS_EVT_DATA
        });
        server.addHandler(&ws);
        diagnostics::set_log_sinks(send_diagnostics_event, send_diagnostics_log_reset);
        diagnostics::set_timeline_sink(broadcast_filament_timeline);

        // 设置未找到路径的处理
        server.onNotFound([](AsyncWebServerRequest* request) {
            request->send(404, "text/plain", "404: Not found");
        });

        // 启动服务器
        server.begin();
        diagnostics::write(diagnostics::level::info, diagnostics::log_module::web,
                           "HTTP_STARTED", "HTTP 服务器已启动");
    }


    {// 打印机Mqtt配置
        if (MQTT_pass != "") {// 有旧数据,可以先连MQTT
            mesp::Mqttclient Mqtt(mqtt_server(bambu_ip), mqtt_username, MQTT_pass,
                                  callback_fun, mqtt_state_changed);
            diagnostics::write(diagnostics::level::info, diagnostics::log_module::mqtt,
                               "MQTT_CONNECT_REQUEST", "正在使用已保存配置连接 MQTT");
            Mqtt.wait();
            if (Mqtt.connected()) {
                diagnostics::write(diagnostics::level::info, diagnostics::log_module::mqtt,
                                   "MQTT_READY", "MQTT 连接成功，开始订阅打印机状态");
                MQTT_done = true;
                work(Mqtt);
            } else {
                MQTT_done = false;
                diagnostics::write(diagnostics::level::error, diagnostics::log_module::mqtt,
                                   "MQTT_CONNECT_FAILED", "MQTT 初始连接失败");
            }
        }//if (MQTT_pass != "")
        else {
            diagnostics::mqtt_metrics.mark_not_configured();
            diagnostics::write(diagnostics::level::info, diagnostics::log_module::mqtt,
                               "MQTT_NOT_CONFIGURED", "MQTT 尚未配置");
        }

        while (!MQTT_done) {
            mqtt_Signal.acquire();// 等待mqtt配置
            mesp::Mqttclient Mqtt(mqtt_server(bambu_ip), mqtt_username, MQTT_pass,
                                  callback_fun, mqtt_state_changed);
            diagnostics::write(diagnostics::level::info, diagnostics::log_module::mqtt,
                               "MQTT_CONNECT_REQUEST", "收到配置，正在连接 MQTT");
            Mqtt.wait();
            if (Mqtt.connected()) {
                diagnostics::write(diagnostics::level::info, diagnostics::log_module::mqtt,
                                   "MQTT_READY", "MQTT 连接成功，开始订阅打印机状态");
                MQTT_done = true;
                work(Mqtt);
            } else {
                MQTT_done = false;
                diagnostics::write(diagnostics::level::error, diagnostics::log_module::mqtt,
                                   "MQTT_CONNECT_FAILED", "MQTT 连接失败");
            }
        }
    }



    int cnt = 0;
    while (true) {
        mstd::delay(20000ms);
        // esp::gpio_out(esp::LED_R, cnt % 2);
        ++cnt;
    }
    return;
}
