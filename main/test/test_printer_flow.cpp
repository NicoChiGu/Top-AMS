#include <ArduinoJson.h>

#include "bambu.hpp"
#include "filament_change_trigger.hpp"
#include "filament_flow_policy.hpp"
#include "motor_interlock.hpp"
#include "printer_protocol.hpp"
#include "printer_status.hpp"
#include "unity.h"

TEST_CASE("AMS status decoder covers A1 mini filament steps", "[printer][ams]") {
    TEST_ASSERT_FALSE(printer_status::decode_ams_status(-1).known);
    TEST_ASSERT_TRUE(printer_status::decode_ams_status(0).known);
    for (int raw = 256; raw <= 270; ++raw) {
        const auto decoded = printer_status::decode_ams_status(raw);
        TEST_ASSERT_TRUE(decoded.known);
        TEST_ASSERT_EQUAL_UINT8(1, decoded.main);
        TEST_ASSERT_EQUAL_UINT8(raw - 256, decoded.sub);
    }

    const auto assist = printer_status::decode_ams_status(768);
    TEST_ASSERT_TRUE(assist.known);
    TEST_ASSERT_EQUAL_STRING("辅助送料/料路已啮合（不是进料完成）",
                             assist.label_zh.c_str());
    for (const int known_main : {0x0200, 0x0400, 0x0700, 0x1000, 0x2000})
        TEST_ASSERT_TRUE(printer_status::decode_ams_status(known_main).known);

    const auto unknown = printer_status::decode_ams_status(1234);
    TEST_ASSERT_FALSE(unknown.known);
    TEST_ASSERT_EQUAL_UINT8(4, unknown.main);
    TEST_ASSERT_EQUAL_UINT8(210, unknown.sub);
    TEST_ASSERT_EQUAL_STRING("未知状态：主码 4，子码 210", unknown.label_zh.c_str());
}

TEST_CASE("printer faults reconstruct compact HMS and known Chinese actions",
          "[printer][fault]") {
    TEST_ASSERT_EQUAL_STRING("07FF8010",
        printer_status::short_hms_code(0x07FF0000u, 0x00018010u).c_str());

    const auto jam = printer_status::decode_hms(0x07FF0000u, 0x00018010u);
    TEST_ASSERT_TRUE(jam.active);
    TEST_ASSERT_TRUE(jam.interlock);
    TEST_ASSERT_TRUE(jam.known);
    TEST_ASSERT_EQUAL_STRING("外挂料盘或耗材卡住", jam.label_zh.c_str());

    for (const uint32_t code : {0x00018007u, 0x0001C00Au}) {
        const auto fault = printer_status::decode_hms(0x07FF0000u, code);
        TEST_ASSERT_TRUE(fault.known);
        TEST_ASSERT_TRUE(fault.label_zh.find("喷嘴") != std::string::npos);
    }

    const auto unknown = printer_status::decode_print_error(12345u);
    TEST_ASSERT_FALSE(unknown.known);
    TEST_ASSERT_TRUE(unknown.display_code.find("0x") != std::string::npos);
}

TEST_CASE("G-code payload serializes multiline quotes and backslashes",
          "[printer][protocol]") {
    const std::string source = "G1 X1\\path\r\nM117 \"ready\"\n\n";
    const std::string payload = bambu::msg::runGcode(source, 42);
    JsonDocument parsed;
    TEST_ASSERT_FALSE(deserializeJson(parsed, payload));
    TEST_ASSERT_EQUAL_STRING("gcode_line", parsed["print"]["command"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("42", parsed["print"]["sequence_id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("G1 X1\\path\nM117 \"ready\"\n",
                             parsed["print"]["param"].as<const char*>());
}

TEST_CASE("sequence acknowledgements reject stale and different replies",
          "[printer][protocol]") {
    printer_protocol::acknowledgement_tracker tracker;
    const uint32_t before = tracker.version();
    tracker.observe(7);
    TEST_ASSERT_TRUE(tracker.observed_after(7, before));

    const uint32_t after_first = tracker.version();
    TEST_ASSERT_FALSE(tracker.observed_after(7, after_first));
    tracker.observe(6);
    TEST_ASSERT_FALSE(tracker.observed_after(7, after_first));

    const uint32_t before_interleaved = tracker.version();
    tracker.observe(8);
    tracker.observe(0); // 周期状态包不得覆盖刚收到的目标回执。
    TEST_ASSERT_TRUE(tracker.observed_after(8, before_interleaved));

    TEST_ASSERT_FALSE(printer_protocol::fresh_state_transition_matches(260, 260, 20, 20));
    TEST_ASSERT_FALSE(printer_protocol::fresh_state_transition_matches(0, 260, 21, 20));
    TEST_ASSERT_TRUE(printer_protocol::fresh_state_transition_matches(260, 260, 21, 20));

    printer_protocol::consecutive_sample_gate temperature_gate(2, 10);
    TEST_ASSERT_FALSE(temperature_gate.update(10, true));
    TEST_ASSERT_FALSE(temperature_gate.update(11, true));
    TEST_ASSERT_FALSE(temperature_gate.update(11, true));
    TEST_ASSERT_TRUE(temperature_gate.update(12, true));
    printer_protocol::consecutive_sample_gate reset_gate(2, 20);
    TEST_ASSERT_FALSE(reset_gate.update(21, true));
    TEST_ASSERT_FALSE(reset_gate.update(22, false));
    TEST_ASSERT_FALSE(reset_gate.update(23, true));
    TEST_ASSERT_TRUE(reset_gate.update(24, true));
}

TEST_CASE("motor arbiter and assist debounce are fail closed", "[motor][safety]") {
    motor_interlock::arbiter arbiter;
    TEST_ASSERT_TRUE(arbiter.try_claim(motor_interlock::owner::load));
    TEST_ASSERT_FALSE(arbiter.try_claim(motor_interlock::owner::assist));
    arbiter.request_stop();
    TEST_ASSERT_TRUE(arbiter.stop_requested());
    arbiter.release(motor_interlock::owner::load);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(motor_interlock::owner::none),
                          static_cast<int>(arbiter.current()));

    motor_interlock::falling_edge_debouncer debounce(5);
    for (int i = 0; i < 20; ++i)
        TEST_ASSERT_FALSE(debounce.update(true));
    for (int i = 0; i < 5; ++i)
        TEST_ASSERT_FALSE(debounce.update(false));
    for (int i = 0; i < 4; ++i)
        TEST_ASSERT_FALSE(debounce.update(true));
    TEST_ASSERT_TRUE(debounce.update(true));
    for (int i = 0; i < 20; ++i)
        TEST_ASSERT_FALSE(debounce.update(true));
    for (int i = 0; i < 5; ++i)
        TEST_ASSERT_FALSE(debounce.update(false));
    for (int i = 0; i < 4; ++i)
        TEST_ASSERT_FALSE(debounce.update(true));
    TEST_ASSERT_TRUE(debounce.update(true));
}

TEST_CASE("flow policy never resumes without every final evidence",
          "[filament][safety]") {
    filament_flow_policy::resume_evidence evidence{
        .printer_status_ready = true,
        .mqtt_fresh = true,
        .fault_free = true,
        .sensor_present = true,
        .channel_confirmed = true,
        .current_channel = 3,
        .target_channel = 3,
    };
    TEST_ASSERT_TRUE(filament_flow_policy::can_resume(evidence));
    evidence.mqtt_fresh = false;
    TEST_ASSERT_FALSE(filament_flow_policy::can_resume(evidence));
    evidence.mqtt_fresh = true;
    evidence.sensor_present = false;
    TEST_ASSERT_FALSE(filament_flow_policy::can_resume(evidence));

    TEST_ASSERT_EQUAL_INT(2, filament_flow_policy::channel_after_attempt(2, 3, false, false));
    TEST_ASSERT_EQUAL_INT(0, filament_flow_policy::channel_after_attempt(2, 3, true, false));
    TEST_ASSERT_EQUAL_INT(3, filament_flow_policy::channel_after_attempt(2, 3, true, true));

    TEST_ASSERT_EQUAL_INT(0,
        filament_flow_policy::confirmed_startup_channel(1, false, 4));
    TEST_ASSERT_EQUAL_INT(0,
        filament_flow_policy::confirmed_startup_channel(5, true, 4));
    TEST_ASSERT_EQUAL_INT(3,
        filament_flow_policy::confirmed_startup_channel(3, true, 4));
}

TEST_CASE("trigger decoder separates ordinary and initial-load marker ranges",
          "[filament][trigger]") {
    filament_change::trigger_state state;
    state.update_bed_target(9);
    state.update_gcode_state("PAUSE");
    const auto request = state.request(8);
    TEST_ASSERT_TRUE(request.valid);
    TEST_ASSERT_EQUAL_INT(1, request.channel);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(filament_change::trigger_kind::initial_load),
                          static_cast<int>(request.kind));

    state.update_bed_target(13);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(filament_change::trigger_readiness::invalid_channel),
                          static_cast<int>(state.evaluate(4)));
}
