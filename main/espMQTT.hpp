#pragma once
#include "config.hpp"
#include "diagnostics.hpp"
#include "tools.hpp"

#include "espIO.hpp"
#include "esp_ota_ops.h"
#include "esp_tls.h"
#include "mqtt_client.h"

namespace mesp {

    using std::string;

    using callback_fun_ptr = void (*)(esp_mqtt_client_handle_t, const string&);
    using state_callback_fun_ptr = void (*)(int);

    struct Mqttclient {
      private:
        static void mqtt_event_callback(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
            // fpr("\nMqtt事件分发");
            esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(event_data);
            esp_mqtt_client_handle_t client = event->client;
            // int msg_id = -1;

            Mqttclient& This = *static_cast<Mqttclient*>(handler_args);
            callback_fun_ptr f = This.event_data_fun;

            switch (esp_mqtt_event_id_t(event_id)) {
            case MQTT_EVENT_CONNECTED:
                diagnostics::mqtt_metrics.mark_connected();
                diagnostics::write(diagnostics::level::info, diagnostics::log_module::mqtt,
                                   "MQTT_CONNECTED", "MQTT 会话已建立");
                This.set_state(mqtt_state::connected);
                break;
            case MQTT_EVENT_DISCONNECTED:
                diagnostics::mqtt_metrics.mark_disconnected();
                diagnostics::write(diagnostics::level::warning, diagnostics::log_module::mqtt,
                                   "MQTT_DISCONNECTED", "MQTT 已断开，将自动尝试重连");
                This.set_state(mqtt_state::disconnected);
                break;
            case MQTT_EVENT_BEFORE_CONNECT:
                diagnostics::mqtt_metrics.mark_connecting();
                diagnostics::write(diagnostics::level::debug, diagnostics::log_module::mqtt,
                                   "MQTT_CONNECTING", "MQTT 正在尝试连接");
                // Keep the internal state at init until a terminal connection
                // event wakes wait(); only the diagnostic/UI state is changing.
                if (This.state_event_fun != nullptr)
                    This.state_event_fun(mqtt_state::connecting);
                break;
            case MQTT_EVENT_SUBSCRIBED:
                diagnostics::write(diagnostics::level::info, diagnostics::log_module::mqtt,
                                   "MQTT_SUBSCRIBED",
                                   "MQTT 订阅成功，消息 ID " + std::to_string(event->msg_id));
                break;
            case MQTT_EVENT_UNSUBSCRIBED:
                diagnostics::write(diagnostics::level::debug, diagnostics::log_module::mqtt,
                                   "MQTT_UNSUBSCRIBED", "MQTT 已取消订阅");
                break;
            case MQTT_EVENT_PUBLISHED:
                diagnostics::write(diagnostics::level::debug, diagnostics::log_module::mqtt,
                                   "MQTT_PUBLISHED",
                                   "MQTT 消息已发布，消息 ID " + std::to_string(event->msg_id));
                break;
            case MQTT_EVENT_DATA:
                fpr("收到 MQTT 数据，长度: ", event->data_len);
                if (diagnostics::mqtt_metrics.mark_received()) {
                    diagnostics::write(diagnostics::level::info, diagnostics::log_module::mqtt,
                                       "MQTT_DATA_RECOVERED", "MQTT 数据流已恢复");
                }
                f(client, string(event->data, event->data_len));
                break;
            case MQTT_EVENT_ERROR:
                if (event->error_handle) {
                    if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                        diagnostics::mqtt_metrics.mark_error(
                            "tcp_transport", event->error_handle->esp_tls_last_esp_err,
                            event->error_handle->esp_tls_stack_err,
                            event->error_handle->esp_transport_sock_errno,
                            "MQTT TCP/TLS 传输错误");
                        diagnostics::write(diagnostics::level::error, diagnostics::log_module::mqtt,
                                           "MQTT_TCP_ERROR", "MQTT TCP/TLS 传输错误");
                    } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                        diagnostics::mqtt_metrics.mark_error(
                            "connection_refused", event->error_handle->connect_return_code,
                            0, 0, "MQTT 连接被打印机拒绝");
                        diagnostics::write(diagnostics::level::error, diagnostics::log_module::mqtt,
                                           "MQTT_REFUSED", "MQTT 连接被打印机拒绝");
                    } else {
                        diagnostics::mqtt_metrics.mark_error(
                            "unknown", event->error_handle->error_type, 0, 0,
                            "未知 MQTT 错误");
                        diagnostics::write(diagnostics::level::error, diagnostics::log_module::mqtt,
                                           "MQTT_ERROR", "发生未知 MQTT 错误");
                    }
                } else {
                    diagnostics::mqtt_metrics.mark_error("unknown", 0, 0, 0, "MQTT 事件错误");
                    diagnostics::write(diagnostics::level::error, diagnostics::log_module::mqtt,
                                       "MQTT_ERROR", "MQTT 事件错误");
                }
                This.set_state(mqtt_state::error);
                break;
            default:
                break;
            }
        }// mqtt_event_callback
      public:
        esp_mqtt_client_handle_t client = nullptr;
        const callback_fun_ptr event_data_fun;
        const state_callback_fun_ptr state_event_fun;

        struct mqtt_state {
            constexpr static int init = 0;
            constexpr static int destroy = 1;
            constexpr static int connected = 2;
            constexpr static int disconnected = 3;
            constexpr static int error = 4;
            constexpr static int connecting = 5;
        };// mqtt_state

        std::atomic<int> state = mqtt_state::init;

  Mqttclient(const string& server, const string& user, const string& pass, callback_fun_ptr f,
             state_callback_fun_ptr state_callback = nullptr)
    : event_data_fun(f), state_event_fun(state_callback)
{
    diagnostics::mqtt_metrics.mark_connecting();
    if (state_event_fun != nullptr)
        state_event_fun(mqtt_state::init);

    esp_mqtt_client_config_t mqtt_cfg{};
    mqtt_cfg.broker.address.uri = server.c_str();
    mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
    mqtt_cfg.credentials.username = user.c_str();
    mqtt_cfg.credentials.authentication.password = pass.c_str();

    // 会话配置
    mqtt_cfg.session.keepalive = 60;              // 心跳间隔60秒（Keep-Alive）
    mqtt_cfg.session.disable_clean_session = false; // 启用clean session
    mqtt_cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1; // 使用MQTT 3.1.1协议
    
    // 网络配置
    mqtt_cfg.network.reconnect_timeout_ms = 5000; // 5秒重连超时
    mqtt_cfg.network.timeout_ms = 10000;          // 10秒网络超时
    mqtt_cfg.network.disable_auto_reconnect = false; // 启用自动重连（默认启用）
    
    // 缓冲区配置
    mqtt_cfg.buffer.size = 4096;                  // 增大接收缓冲区
    mqtt_cfg.buffer.out_size = 2048;              // 发送缓冲区
    
    // 任务配置
    mqtt_cfg.task.stack_size = 6144;              // 增大任务栈
    mqtt_cfg.task.priority = 5;                   // 提高任务优先级

    diagnostics::write(diagnostics::level::info, diagnostics::log_module::mqtt,
                       "MQTT_INIT", "初始化 MQTT 客户端，服务器 " + server);

    client = esp_mqtt_client_init(&mqtt_cfg);
    error_check(esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, Mqttclient::mqtt_event_callback, this), "Mqtt注册事件失败");
    error_check(esp_mqtt_client_start(client), "Mqtt初始化失败");
}
        ~Mqttclient() {
            if (state != mqtt_state::init)
                error_check(esp_mqtt_client_stop(client));
            error_check(esp_mqtt_client_destroy(client));
        }

        Mqttclient(const Mqttclient&) = delete;
        Mqttclient& operator=(const Mqttclient&) = delete;
        Mqttclient(Mqttclient&& r) noexcept
            : event_data_fun(r.event_data_fun), state_event_fun(r.state_event_fun) {
            client = r.client;
            state.store(r.state);
            r.client = nullptr;
            r.state = mqtt_state::init;
        }
        Mqttclient& operator=(Mqttclient&&) noexcept = delete;
        // 先都delete,不然还要加引用计数

        operator esp_mqtt_client_handle_t&() noexcept {
            return client;
        }
        operator const esp_mqtt_client_handle_t&() const noexcept {
            return client;
        }

        // 订阅主题
        void subscribe(const string& topic, int Qos = 1) {
            esp_mqtt_client_subscribe(client, topic.c_str(), Qos);
        }

        void error_check(esp_err_t err, const string& msg = "MQTT错误:") {
            if (err != ESP_OK) {
                diagnostics::mqtt_metrics.mark_error("client", err, 0, 0, msg);
                diagnostics::write(diagnostics::level::error, diagnostics::log_module::mqtt,
                                   "MQTT_CLIENT_ERROR", msg + std::to_string(err));
                set_state(mqtt_state::error);
            }
        }

        // 等待状态变换
        void wait() {
            state.wait(mqtt_state::init);
        }

        // mqtt已连接
        bool connected() const noexcept {
            return state == mqtt_state::connected;
        }

      private:
        void set_state(int value, bool notify_waiters = true) {
            state.store(value);
            if (notify_waiters)
                state.notify_all();
            if (state_event_fun != nullptr)
                state_event_fun(value);
        }

    };// Mqttclinet


    //mqtt连接错误这边,应该是外部给一个错误处理的回调,然后在这个回调里改变mqtt_done@_@

}// mesp
