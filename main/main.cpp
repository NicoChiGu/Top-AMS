#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <chrono>

#include "esptools.hpp"
#include "filament_change_trigger.hpp"

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
volatile int hw_switch = 0;
TaskHandle_t Task2_handle;//延时调试信息


//分割

int bed_target_temper_max = 0;
mesp::wsStoreValue<int> extruder("extruder", 0);// 1-16, 0表示无耗材，默认未设置

int sequence_id = -1;
std::atomic<int> print_error = 0;//打印错误代码用于判断自动续料 50364437是A1 50364420是P1
std::atomic<int> ams_status = -1;
std::atomic<bool> pause_lock{false};// 暂停锁
filament_change::trigger_state change_trigger;
filament_change::trigger_readiness last_change_trigger_readiness = filament_change::trigger_readiness::idle;
bool change_trigger_busy_logged = false;
std::atomic<int> nozzle_target_temper = -1;
//std::atomic<int> hw_switch{0};//小绿点, 其实是布尔

// 系统状态变量，用于前端显示和按钮控制
mesp::wsValue<int> motor_running("motor_running", 0);// 当前运行的电机通道，0表示无电机运行
mesp::wsValue<string> operation_status("operation_status", "idle");// 操作状态: "idle", "changing", "loading"
mesp::wsValue<bool> system_locked("system_locked", false);// 系统锁定状态

inline constexpr int 正常 = 0;
inline constexpr int 退料完成需要退线 = 260;//A1
//inline constexpr int 退料完成需要退线 = 259;//P1
inline constexpr int 退料完成 = 0;// 同正常
inline constexpr int 进料检查 = 262;
inline constexpr int 进料冲刷 = 263;// 推测
inline constexpr int 进料完成 = 768;
//@_@应该放在一个枚举类里

AsyncWebServer server(80);
// AsyncWebSocket ws("/ws");
AsyncWebSocket& ws = mesp::ws_server;//先直接用全局的ws_server

inline mstd::channel_lock<std::function<void()>> async_channel;//异步任务通道

// RAII状态管理类，确保在异常或提前返回时也能清理状态
struct SystemStateGuard {
    bool active;
    SystemStateGuard() : active(true) {
        system_locked = true;
    }
    ~SystemStateGuard() {
        if (active) {
            system_locked = false;
            operation_status = "idle";
            pause_lock = false;
        }
    }
    void release() {
        active = false;
        system_locked = false;
        operation_status = "idle";
        pause_lock = false;
    }
};


inline void filament_log(diagnostics::level severity, std::string_view code, const string& str) {
    diagnostics::write(severity, diagnostics::log_module::filament, code, str);
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
    system["current_channel"] = extruder.get_value();
    system["motor_running"] = motor_running.get_value();
    system["ams_status"] = ams_status.load();
    system["hw_switch"] = hw_switch;
    system["nozzle_target_c"] = nozzle_target_temper.load();

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
           operation_status.get_value() == "idle" &&
           motor_running.get_value() == 0;
}

// @brief 控制电机运行(前向或后向)
// @param moter_id 电机编号,从 1 开始
// @param fwd 标识方向，true 表示前向，false 表示后向
// @param t 延时
template <typename T>
inline void motor_run(int motor_id, bool fwd, T&& t) {
    if (motor_id < 1 || motor_id > config::motors.size()) {
        diagnostics::write(diagnostics::level::error, diagnostics::log_module::motor,
                           "MOTOR_INVALID_CHANNEL",
                           "电机编号错误: " + std::to_string(motor_id));
        return;
    }
    motor_id--;
    if (config::motors[motor_id].forward == config::LED_R) [[unlikely]] {
        config::LED_R = GPIO_NUM_NC;
        config::LED_L = GPIO_NUM_NC;
    }//使用到了通道7,关闭代码中的LED控制

    auto& motor = config::motors[motor_id];

    // 更新电机运行状态
    motor_running = motor_id + 1; // 恢复为1-based索引

    // 运行开始后快照配置，确保一次动作始终使用同一个物理方向。
    const bool reverse_output = motor.reverse_output.get_value();
    const bool physical_forward = fwd != reverse_output;
    const gpio_num_t active_gpio = physical_forward ? motor.forward : motor.backward;
    const gpio_num_t inactive_gpio = physical_forward ? motor.backward : motor.forward;

    diagnostics::write(diagnostics::level::info, diagnostics::log_module::motor,
                       "MOTOR_STARTED", std::string("电机 ") + std::to_string(motor_id + 1) +
                       (fwd ? " 正转" : " 反转") + (reverse_output ? "（反向输出）" : ""));

    // 保证另一方向保持低电平，再驱动本次动作选择的GPIO。
    esp::gpio_out(inactive_gpio, false);
    esp::gpio_out(active_gpio, true);
    mstd::delay(std::forward<T>(t));// 使用传入的延时
    esp::gpio_out(active_gpio, false);
    
    // 电机运行完成，清除状态
    motor_running = 0;
    diagnostics::write(diagnostics::level::info, diagnostics::log_module::motor,
                       "MOTOR_FINISHED", "电机 " + std::to_string(motor_id + 1) + " 运行完成");
}//motor_run


// @brief 控制电机运行(前向或后向)
// @param moter_id 电机编号,从 1 开始
// @param fwd 标识方向，true 表示前向，false 表示后向
inline void motor_run(int motor_id, bool fwd) {
    motor_run(motor_id, fwd,
              fwd ? config::motors[motor_id - 1].load_time.get_value() : config::motors[motor_id - 1].uload_time.get_value());
}//motor_run





//@brief 发布消息到MQTT服务器
int publish(esp_mqtt_client_handle_t client, const std::string& msg) {
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




//换料
void change_filament(esp_mqtt_client_handle_t client, int old_extruder, int new_extruder) {
    SystemStateGuard state_guard;
    operation_status = "changing";
    diagnostics::write(diagnostics::level::info, diagnostics::log_module::filament,
                       "CHANGE_STARTED",
                       "开始换料: 通道 " + std::to_string(old_extruder) + " → " +
                       std::to_string(new_extruder));
    
    publish(client, bambu::msg::get_status);
    mstd::delay(3s);
    
    bool has_filament = (hw_switch == 1);
    
    if (old_extruder == new_extruder && has_filament) {
        diagnostics::filament_history.skip(diagnostics::filament_stage::unload, "同通道无需退料");
        diagnostics::filament_history.skip(diagnostics::filament_stage::retract, "同通道无需退线");
        diagnostics::filament_history.skip(diagnostics::filament_stage::heat, "同通道无需重新加热");
        diagnostics::filament_history.skip(diagnostics::filament_stage::feed, "同通道无需进线");
        diagnostics::filament_history.start(diagnostics::filament_stage::resume, "提交恢复打印命令");
        extruder = new_extruder;
        const int resume_id = publish(client, bambu::msg::print_resume);
        if (resume_id < 0) {
            diagnostics::filament_history.fail(diagnostics::filament_stage::resume,
                                                "恢复命令提交失败", false);
        } else {
            diagnostics::filament_history.finish_stage(diagnostics::filament_stage::resume,
                                                        diagnostics::stage_state::success,
                                                        "恢复命令已提交");
            diagnostics::filament_history.finish_run(diagnostics::run_state::skipped);
        }
        diagnostics::write(diagnostics::level::info, diagnostics::log_module::filament,
                           "CHANGE_SKIPPED", "当前通道正在使用，无需换料");
        state_guard.release();
        return;
    }
    
    if (old_extruder > 0 && old_extruder <= config::motors.size()) {
        if (!has_filament) {
            diagnostics::filament_history.skip(diagnostics::filament_stage::unload,
                                                "微动未检测到耗材，跳过打印机退料");
            diagnostics::filament_history.start(diagnostics::filament_stage::retract,
                                                 "旧通道直接退线");
            motor_run(old_extruder, false);
            diagnostics::filament_history.finish_stage(diagnostics::filament_stage::retract,
                                                        diagnostics::stage_state::success,
                                                        "旧通道退线完成");
        } else {
            if (config::motors[old_extruder - 1].uload_time.get_value() > 0) {
                diagnostics::filament_history.start(diagnostics::filament_stage::unload,
                                                     "等待打印机退料状态");
                // Do not let a retained 260 from an earlier printer operation
                // satisfy this unload before the new command has produced a state.
                ams_status.store(-1);
                const int unload_id = publish(client, bambu::msg::runGcode(
                    "M109 S" + std::to_string(config::motors[old_extruder - 1].temper.get_value()) +
                    "\nM620 S255\nT255\nM621 S255\n"));//新的快速退料
                if (unload_id < 0) {
                    diagnostics::filament_history.fail(diagnostics::filament_stage::unload,
                                                        "退料命令提交失败", false);
                    diagnostics::write(diagnostics::level::error, diagnostics::log_module::filament,
                                       "UNLOAD_PUBLISH_FAILED", "退料命令提交失败，终止本次换料");
                    return;
                }
                if (!mstd::atomic_wait_un_timeout(ams_status, 退料完成需要退线, 120s)) {
                    diagnostics::filament_history.fail(diagnostics::filament_stage::unload,
                                                        "等待退料完成超时", true);
                    diagnostics::write(diagnostics::level::error, diagnostics::log_module::filament,
                                       "UNLOAD_TIMEOUT", "等待退料完成超时，可能打印机未响应");
                    return;
                }
                diagnostics::filament_history.finish_stage(diagnostics::filament_stage::unload,
                                                            diagnostics::stage_state::success,
                                                            "打印机退料完成");

                diagnostics::filament_history.start(diagnostics::filament_stage::retract,
                                                     "电机退出旧通道料线");
                motor_run(old_extruder, false);

                if (!mstd::atomic_wait_un_timeout(ams_status, 退料完成, 30s)) {
                    diagnostics::filament_history.warning_timeout(
                        diagnostics::filament_stage::retract,
                        "退线后等待正常状态超时，流程继续");
                    diagnostics::write(diagnostics::level::warning, diagnostics::log_module::filament,
                                       "RETRACT_STATE_TIMEOUT", "退线后等待正常状态超时，继续执行");
                } else {
                    diagnostics::filament_history.finish_stage(diagnostics::filament_stage::retract,
                                                                diagnostics::stage_state::success,
                                                                "退线完成并恢复正常状态");
                }
            } else {
                diagnostics::filament_history.skip(diagnostics::filament_stage::unload,
                                                    "旧通道未配置退线时间");
                diagnostics::filament_history.skip(diagnostics::filament_stage::retract,
                                                    "旧通道退线时间为零");
            }
        }
    } else if (old_extruder == 0) {
        diagnostics::filament_history.skip(diagnostics::filament_stage::unload,
                                            "当前没有已记录耗材");
        diagnostics::filament_history.skip(diagnostics::filament_stage::retract,
                                            "当前没有旧通道料线");
    } else {
        diagnostics::filament_history.skip(diagnostics::filament_stage::unload,
                                            "旧通道记录无效，跳过退料");
        diagnostics::filament_history.skip(diagnostics::filament_stage::retract,
                                            "旧通道记录无效，跳过退线");
        diagnostics::write(diagnostics::level::warning, diagnostics::log_module::filament,
                           "OLD_CHANNEL_INVALID", "旧通道记录无效，直接尝试目标通道进料");
    }
    
    if (config::motors[new_extruder - 1].load_time > 0) {//使用固定时间进料@_@
        int new_nozzle_temper = config::motors[new_extruder - 1].temper.get_value();
        diagnostics::filament_history.start(diagnostics::filament_stage::heat,
                                             "等待热端达到 " + std::to_string(new_nozzle_temper) + "°C");
        publish(client, bambu::msg::runGcode("M109 S" + std::to_string(new_nozzle_temper)));
        auto temp_deadline = std::chrono::steady_clock::now() + 300s; // 5分钟超时
        while (nozzle_target_temper.load() < new_nozzle_temper - 5) {
            if (std::chrono::steady_clock::now() > temp_deadline) {
                diagnostics::filament_history.fail(diagnostics::filament_stage::heat,
                                                    "等待热端温度超时", true);
                diagnostics::write(diagnostics::level::error, diagnostics::log_module::filament,
                                   "HEAT_TIMEOUT", "等待热端温度超时，可能打印机未响应");
                return;
            }
            mstd::delay(500ms);
        }
        diagnostics::filament_history.finish_stage(diagnostics::filament_stage::heat,
                                                    diagnostics::stage_state::success,
                                                    "热端已达到进料条件");

        diagnostics::filament_history.start(diagnostics::filament_stage::feed,
                                             "电机推进目标通道料线");
        publish(client, bambu::msg::runGcode("G1 E150 F500"));//旋转热端齿轮辅助进料
        mstd::delay(3s);
        motor_run(new_extruder, true);
        diagnostics::filament_history.finish_stage(diagnostics::filament_stage::feed,
                                                    diagnostics::stage_state::success,
                                                    "目标通道进线完成");

        extruder = new_extruder;

        diagnostics::filament_history.start(diagnostics::filament_stage::resume,
                                             "提交恢复打印命令");
        const int resume_id = publish(client, bambu::msg::print_resume);
        if (resume_id < 0) {
            diagnostics::filament_history.fail(diagnostics::filament_stage::resume,
                                                "恢复命令提交失败", false);
            diagnostics::write(diagnostics::level::error, diagnostics::log_module::filament,
                               "RESUME_FAILED", "换料完成，但恢复命令提交失败");
            return;
        }
        diagnostics::filament_history.finish_stage(diagnostics::filament_stage::resume,
                                                    diagnostics::stage_state::success,
                                                    "恢复命令已提交");
        diagnostics::filament_history.finish_run(diagnostics::run_state::success);
        diagnostics::write(diagnostics::level::info, diagnostics::log_module::filament,
                           "CHANGE_FINISHED", "换料完成: 通道 " + std::to_string(new_extruder));
    } else {//自动判定进料时间
        diagnostics::filament_history.skip(diagnostics::filament_stage::heat,
                                            "自动判定进料模式未实现");
        diagnostics::filament_history.fail(diagnostics::filament_stage::feed,
                                            "自动判定进料模式未实现", false);
        diagnostics::write(diagnostics::level::error, diagnostics::log_module::filament,
                           "AUTO_FEED_UNAVAILABLE", "自动判定进料功能未实现，无法完成换料");
        state_guard.release();
        return;
    }
    
    state_guard.release();
}// change_filament
/*
 * 似乎外挂托盘的数据也能通过mqtt改动
 */


esp_mqtt_client_handle_t __client;

//上料
void load_filament(int new_extruder) {
    // __client;//先用这个,之后解耦出来,应该穿参进来,改好clien的生存期和错误回报就行

    // 检查系统是否被锁定（换料或上料进行中）
    if (system_locked.get_value() || pause_lock.load()) {
        filament_log(diagnostics::level::warning, "LOAD_BUSY", "系统正在执行其他操作，请稍后再试");
        return;
    }

    // 检查__client是否有效
    if (__client == nullptr) {
        filament_log(diagnostics::level::error, "LOAD_MQTT_UNAVAILABLE", "MQTT 客户端未初始化，无法执行上料");
        return;
    }

    if (!(new_extruder > 0 && new_extruder <= config::motors.size())) {
        filament_log(diagnostics::level::error, "LOAD_INVALID_CHANNEL", "不支持的上料通道");
        return;
    }

    // 使用RAII确保状态总是被清理
    SystemStateGuard state_guard;
    operation_status = "loading";
    filament_log(diagnostics::level::info, "LOAD_STARTED",
                 "开始手动上料到通道 " + std::to_string(new_extruder));

    {//新写的N20上料
        publish(__client, bambu::msg::get_status);//查询小绿点
        mstd::delay(3s);//等待查询结果
        
        // 进料前检查状态：通过hw_switch_state判断是否有料
        bool has_filament = (hw_switch == 1);
        
        if (has_filament) {//有料需要检查通道
            int old_extruder = extruder.get_value();
            if (old_extruder == 0) {
                filament_log(diagnostics::level::warning, "LOAD_CHANNEL_REQUIRED",
                             "检测到有料但未设置当前通道，请先设置当前通道");
                state_guard.release(); // 提前释放状态
                return;
            }
            if (old_extruder == new_extruder) {
                // 通道一样，跳过进料
                filament_log(diagnostics::level::info, "LOAD_SKIPPED",
                             "当前通道已经是 " + std::to_string(new_extruder) + "，跳过进料");
                state_guard.release(); // 提前释放状态
                return;
            }
            // 通道变更，需要先退料
            filament_log(diagnostics::level::info, "LOAD_REQUIRES_UNLOAD",
                         "检测到有料且通道变更(" + std::to_string(old_extruder) + " → " +
                         std::to_string(new_extruder) + ")，执行退料");
            publish(__client, bambu::msg::runGcode(
                                  "M109 S" + std::to_string(config::motors[old_extruder - 1].temper.get_value()) + "\nM620 S255\nT255\nM621 S255\n"));//新的快速退料
            if (!mstd::atomic_wait_un_timeout(ams_status, 退料完成需要退线, 120s)) {
                filament_log(diagnostics::level::error, "LOAD_UNLOAD_TIMEOUT",
                             "手动上料前等待退料完成超时");
                return; // RAII会自动清理状态
            }
            filament_log(diagnostics::level::info, "LOAD_UNLOAD_FINISHED", "退料完成，开始退线");

            motor_run(old_extruder, false);// 退线

            if (!mstd::atomic_wait_un_timeout(ams_status, 退料完成, 30s)) {
                filament_log(diagnostics::level::warning, "LOAD_RETRACT_STATE_TIMEOUT",
                             "退线后等待正常状态超时，继续执行");
                // 继续执行，不返回，因为退线已完成
            }
            filament_log(diagnostics::level::info, "LOAD_RETRACT_FINISHED", "退线完成");
        } else {
            // 无料，直接进料
            filament_log(diagnostics::level::info, "LOAD_NO_FILAMENT", "检测到无料，直接进料");
        }//if (has_filament)
        {//进料
            int new_nozzle_temper = config::motors[new_extruder - 1].temper.get_value();
            publish(__client, bambu::msg::runGcode("M109 S" + std::to_string(new_nozzle_temper)));
            auto temp_deadline = std::chrono::steady_clock::now() + 300s; // 5分钟超时
            while (nozzle_target_temper.load() < new_nozzle_temper - 5) {
                if (std::chrono::steady_clock::now() > temp_deadline) {
                    filament_log(diagnostics::level::error, "LOAD_HEAT_TIMEOUT",
                                 "手动上料等待热端温度超时");
                    return; // RAII会自动清理状态
                }
                mstd::delay(500ms);// 等待热端温度达到目标温度
            }
            // mstd::delay(5s);//先5s,时间可能取决于热端到250的速度,一个想法是把拉高热端提前能省点时间,但是比较难控制
            //@_@也可以读热端温度,不过如果读==250的话,肯定是挤出机先转,或者可以考虑条件为>240之类

            filament_log(diagnostics::level::info, "LOAD_FEEDING", "开始进线");
            publish(__client, bambu::msg::runGcode("G1 E150 F500"));//旋转热端齿轮辅助进料
            mstd::delay(3s);//还是需要延迟,命令落实没这么快
            motor_run(new_extruder, true);// 进线

            /*
            此处应该查下小绿点,如果小绿点没触发的话,G1命令无效
            */

            extruder = new_extruder;//换料完成
            // ws_extruder = std::to_string(new_extruder);// 更新前端显示的耗材编号

            publish(__client,
                    bambu::msg::runGcode(
                        std::string("G1 E100 F180\n")//简单冲刷100
                        + std::string("M400\n") + std::string("M106 P1 S255\n")//风扇全速
                        + std::string("M400 S3\n")//冷却
                        + std::string("G1 X -3.5 F18000\nG1 X -13.5 F3000\nG1 X -3.5 F18000\nG1 X -13.5 F3000\nG1 X -3.5 F18000\nG1 X -13.5 F3000\n")//切屎
                        + std::string("M400\nM106 P1 S0\nM109 S90\n")));//结束并降温到90
        }
        filament_log(diagnostics::level::info, "LOAD_FINISHED",
                     "手动上料完成: 通道 " + std::to_string(new_extruder));
    }//新写的N20上料

    // 正常完成，释放状态
    state_guard.release();
    
    return;

}//load_filament




void work(mesp::Mqttclient& Mqtt) {//之后应该修改好mesp::Mqttclient生命周期@_@
    __client = Mqtt.client;
    Mqtt.subscribe(config::topic_subscribe());// 订阅消息

    // 应用启动时检查小绿点状态并初始化通道
    filament_log(diagnostics::level::info, "FILAMENT_PROBE", "检查挤出机耗材状态");
    publish(__client, bambu::msg::get_status);
    mstd::delay(3s);//等待查询结果
    
    // 根据hw_switch_state判断当前是否有料
    if (hw_switch == 1) {
        // 有料，默认设置为通道1
        if (extruder.get_value() == 0) {
            extruder = 1;
            filament_log(diagnostics::level::info, "FILAMENT_CHANNEL_DEFAULTED",
                         "检测到挤出机有料，默认设置为通道 1");
        } else {
            filament_log(diagnostics::level::info, "FILAMENT_DETECTED",
                         "检测到挤出机有料，当前通道: " + std::to_string(extruder.get_value()));
        }
    } else {
        // 无料，设置为空
        extruder = 0;
        filament_log(diagnostics::level::info, "FILAMENT_EMPTY",
                     "检测到挤出机无料，当前通道设置为空");
    }

    int cnt = 0;
    while (true) {
        mstd::delay(20000ms);
        // esp::gpio_out(esp::LED_R, cnt % 2);
        ++cnt;
    }
}

//@brief MQTT回调函数
void callback_fun(esp_mqtt_client_handle_t client, const std::string& json) {// 接受到信息的回调
    // fpr(json);
    using namespace ArduinoJson;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);
    if (error) {
        diagnostics::write(diagnostics::level::warning, diagnostics::log_module::mqtt,
                           "MQTT_JSON_INVALID", "收到无法解析的 MQTT JSON");
        return;
    }

    // mesp::print_memory_info();

    JsonVariantConst print = doc["print"];
    if (!print["bed_target_temper"].isNull())
        change_trigger.update_bed_target(print["bed_target_temper"].as<int>());
    if (!print["gcode_state"].isNull())
        change_trigger.update_gcode_state(print["gcode_state"].as<std::string>());
    if (!print["nozzle_target_temper"].isNull())
        nozzle_target_temper.store(print["nozzle_target_temper"].as<int>());
    if (!print["hw_switch_state"].isNull())
        hw_switch = print["hw_switch_state"].as<int>();

    // Process AMS status even while a cached PAUSE trigger is busy/consumed.
    // Returning before this update would make an active unload wait miss state 260.
    if (!print["ams_status"].isNull()) {
        const int ams_status_now = print["ams_status"].as<int>();
        if (ams_status.exchange(ams_status_now) != ams_status_now) {
            diagnostics::write(diagnostics::level::debug, diagnostics::log_module::filament,
                               "AMS_STATUS", "AMS 状态更新为 " + std::to_string(ams_status_now));
            ams_status.notify_one();
        }
    }

    const int bed_target_temper = change_trigger.bed_target();
    if (bed_target_temper == 0)
        bed_target_temper_max = 0;// 打印结束
    else if (!(bed_target_temper > 0 && bed_target_temper < 17))
        bed_target_temper_max = std::max(bed_target_temper, bed_target_temper_max);

    const auto readiness = change_trigger.evaluate(config::motors.size());
    if (readiness != last_change_trigger_readiness) {
        if (readiness == filament_change::trigger_readiness::waiting_for_pause) {
            diagnostics::write(diagnostics::level::debug, diagnostics::log_module::filament,
                               "CHANGE_WAITING_FOR_PAUSE",
                               "已收到目标通道 " + std::to_string(bed_target_temper) +
                               "，等待打印机进入 PAUSE");
        } else if (readiness == filament_change::trigger_readiness::waiting_for_channel) {
            diagnostics::write(diagnostics::level::debug, diagnostics::log_module::filament,
                               "CHANGE_WAITING_FOR_CHANNEL",
                               "已收到 PAUSE，等待有效的目标通道标记");
        }
        last_change_trigger_readiness = readiness;
        if (readiness != filament_change::trigger_readiness::ready)
            change_trigger_busy_logged = false;
    }

    if (readiness == filament_change::trigger_readiness::invalid_channel) {
        const int new_extruder = bed_target_temper;
        const int old_extruder = extruder.get_value();
        change_trigger.consume();
        last_change_trigger_readiness = filament_change::trigger_readiness::consumed;
        diagnostics::filament_history.begin(old_extruder, new_extruder,
                                             "检测到无效的暂停换料通道");
        diagnostics::filament_history.finish_stage(diagnostics::filament_stage::trigger,
                                                    diagnostics::stage_state::error,
                                                    "目标通道无效");
        diagnostics::filament_history.skip(diagnostics::filament_stage::unload, "触发失败");
        diagnostics::filament_history.skip(diagnostics::filament_stage::retract, "触发失败");
        diagnostics::filament_history.skip(diagnostics::filament_stage::heat, "触发失败");
        diagnostics::filament_history.skip(diagnostics::filament_stage::feed, "触发失败");
        diagnostics::write(diagnostics::level::error, diagnostics::log_module::filament,
                           "CHANGE_INVALID_CHANNEL",
                           "无效的目标通道: " + std::to_string(new_extruder));
        if (bed_target_temper_max > 0)
            publish(client, bambu::msg::runGcode("M190 S" + std::to_string(bed_target_temper_max)));
        mstd::delay(1000ms);
        diagnostics::filament_history.start(diagnostics::filament_stage::resume,
                                             "提交恢复打印命令");
        const int resume_id = publish(client, bambu::msg::print_resume);
        diagnostics::filament_history.finish_stage(
            diagnostics::filament_stage::resume,
            resume_id < 0 ? diagnostics::stage_state::error : diagnostics::stage_state::success,
            resume_id < 0 ? "恢复命令提交失败" : "恢复命令已提交");
        diagnostics::filament_history.finish_run(diagnostics::run_state::error);
    } else if (readiness == filament_change::trigger_readiness::ready) {
        const int new_extruder = bed_target_temper;
        const int old_extruder = extruder.get_value();

        if (system_locked.get_value() || pause_lock.load()) {
            if (!change_trigger_busy_logged) {
                diagnostics::write(diagnostics::level::warning, diagnostics::log_module::filament,
                                   "CHANGE_BUSY", "系统忙碌，保留当前自动换料请求等待重试");
                change_trigger_busy_logged = true;
            }
        } else if (old_extruder != new_extruder) {
            bool expected = false;
            if (!pause_lock.compare_exchange_strong(expected, true)) {
                if (!change_trigger_busy_logged) {
                    diagnostics::write(diagnostics::level::warning, diagnostics::log_module::filament,
                                       "CHANGE_BUSY", "换料锁已被占用，保留当前请求等待重试");
                    change_trigger_busy_logged = true;
                }
            } else {
                diagnostics::filament_history.begin(
                    old_extruder, new_extruder,
                    "增量 MQTT 状态合并后确认 PAUSE 与目标通道");
                diagnostics::filament_history.finish_stage(diagnostics::filament_stage::trigger,
                                                            diagnostics::stage_state::success,
                                                            "换料触发已确认");
                diagnostics::write(diagnostics::level::info, diagnostics::log_module::filament,
                                   "CHANGE_TRIGGER_ACCEPTED",
                                   "接受自动换料触发: 通道 " + std::to_string(old_extruder) +
                                   " → " + std::to_string(new_extruder) +
                                   "（PAUSE + 热床通道标记）");
                if (bed_target_temper_max > 0)
                    publish(client, bambu::msg::runGcode("M190 S" + std::to_string(bed_target_temper_max)));

                async_channel.emplace([=]() {
                    change_filament(client, old_extruder, new_extruder);
                });
                change_trigger.consume();
                last_change_trigger_readiness = filament_change::trigger_readiness::consumed;
                change_trigger_busy_logged = false;
                diagnostics::write(diagnostics::level::info, diagnostics::log_module::filament,
                                   "CHANGE_QUEUED",
                                   "换料任务已入队: 通道 " + std::to_string(old_extruder) +
                                   " → " + std::to_string(new_extruder));
            }
        } else {
            change_trigger.consume();
            last_change_trigger_readiness = filament_change::trigger_readiness::consumed;
            change_trigger_busy_logged = false;
            diagnostics::filament_history.begin(
                old_extruder, new_extruder,
                "增量 MQTT 状态合并后确认同通道暂停");
            diagnostics::filament_history.finish_stage(diagnostics::filament_stage::trigger,
                                                        diagnostics::stage_state::success,
                                                        "换料触发已确认");
            diagnostics::filament_history.skip(diagnostics::filament_stage::unload,
                                                "同通道无需退料");
            diagnostics::filament_history.skip(diagnostics::filament_stage::retract,
                                                "同通道无需退线");
            diagnostics::filament_history.skip(diagnostics::filament_stage::heat,
                                                "同通道无需重新加热");
            diagnostics::filament_history.skip(diagnostics::filament_stage::feed,
                                                "同通道无需进线");
            if (bed_target_temper_max > 0)
                publish(client, bambu::msg::runGcode("M190 S" + std::to_string(bed_target_temper_max)));
            mstd::delay(1000ms);
            diagnostics::filament_history.start(diagnostics::filament_stage::resume,
                                                 "提交恢复打印命令");
            const int resume_id = publish(client, bambu::msg::print_resume);
            if (resume_id < 0) {
                diagnostics::filament_history.fail(diagnostics::filament_stage::resume,
                                                    "恢复命令提交失败", false);
            } else {
                diagnostics::filament_history.finish_stage(
                    diagnostics::filament_stage::resume,
                    diagnostics::stage_state::success, "恢复命令已提交");
                diagnostics::filament_history.finish_run(diagnostics::run_state::skipped);
            }
            diagnostics::write(diagnostics::level::info, diagnostics::log_module::filament,
                               "CHANGE_SKIPPED", "目标通道与当前通道相同，无需换料");
        }
    }

}// callback

void Task1(void* param) {
    esp::gpio_set_in(config::forward_click);

    while (true) {
        int level = gpio_get_level(config::forward_click);

        if (level == 0) {
            // 检查辅助进料开关是否开启
            if (config::assist_feeding_enabled.get_value() == 1) {
                int now_extruder = extruder.get_value();
                if (now_extruder > 0 && now_extruder <= config::motors.size()) {
                    diagnostics::write(diagnostics::level::info, diagnostics::log_module::motor,
                                       "ASSIST_TRIGGERED", "微动触发，执行辅助进料");
                    motor_run(now_extruder, true, 1s);// 进线
                } else {
                    diagnostics::write(diagnostics::level::warning, diagnostics::log_module::motor,
                                       "ASSIST_NO_CHANNEL", "微动触发，但当前无有效通道");
                }
            } else {
                diagnostics::write(diagnostics::level::debug, diagnostics::log_module::motor,
                                   "ASSIST_DISABLED", "微动触发，但辅助进料已关闭");
            }
        }

        mstd::delay(50ms);
    }
}//微动缓冲程序


//延时检测
void Task2(void* param) {
    while (true) {
        mstd::delay(1000ms);
        if (diagnostics::mqtt_metrics.mark_stale_if_needed()) {
            diagnostics::write(diagnostics::level::warning, diagnostics::log_module::mqtt,
                               "MQTT_DATA_STALE", "MQTT 已超过 8 秒未收到打印机数据");
        }
    }
}

#include "index.hpp"

volatile bool running_flag{false};

extern "C" void app_main() {
    diagnostics::write(diagnostics::level::info, diagnostics::log_module::system,
                       "SYSTEM_BOOT", "Top-AMS 固件启动");
#ifndef LOCAL_CONFIG
    for (size_t i = 0; i < config::motors.size(); i++) {
        auto& x = config::motors[i];
        esp::gpio_out(x.forward, false);
        esp::gpio_out(x.backward, false);
    }//初始化电机GPIO
#endif

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
                        async_channel.emplace(
                            [motor_id]() {
                                motor_run(motor_id, true);
                            });
                    } else if (command == "motor_backward") {//电机前向控制
                        int motor_id = doc["action"]["value"] | -1;
                        async_channel.emplace(
                            [motor_id]() {
                                motor_run(motor_id, false);
                            });
                    } else if (command == "load_filament") {
                        int new_extruder = doc["action"]["value"] | -1;
                        async_channel.emplace(
                            [new_extruder]() {
                                load_filament(new_extruder);
                            });

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
