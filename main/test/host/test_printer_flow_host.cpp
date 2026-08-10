#include <cassert>
#include <iostream>
#include <string>

#include "filament_change_trigger.hpp"
#include "filament_flow_policy.hpp"
#include "motor_interlock.hpp"
#include "printer_protocol.hpp"
#include "printer_status.hpp"

int main() {
    assert(!printer_status::decode_ams_status(-1).known);
    assert(printer_status::decode_ams_status(0).known);
    for (int raw = 256; raw <= 270; ++raw)
        assert(printer_status::decode_ams_status(raw).known);
    assert(printer_status::decode_ams_status(768).label_zh.find("不是进料完成") != std::string::npos);
    const auto unknown = printer_status::decode_ams_status(1234);
    assert(!unknown.known && unknown.main == 4 && unknown.sub == 210);

    assert(printer_status::short_hms_code(0x07FF0000u, 0x00018010u) == "07FF8010");
    assert(printer_status::decode_hms(0x07FF0000u, 0x00018010u).interlock);
    assert(printer_status::decode_hms(0x07FF0000u, 0x00018007u).known);
    assert(printer_status::decode_hms(0x07FF0000u, 0x0001C00Au).known);

    assert(printer_protocol::normalize_gcode("G1 X1\r\nM117 \"ok\"\n\n") ==
           "G1 X1\nM117 \"ok\"\n");
    uint32_t sequence = 0;
    assert(printer_protocol::parse_sequence_id("429", sequence) && sequence == 429);
    assert(!printer_protocol::parse_sequence_id("4x", sequence));
    assert(!printer_protocol::fresh_state_transition_matches(260, 260, 10, 10));
    assert(!printer_protocol::fresh_state_transition_matches(0, 260, 11, 10));
    assert(printer_protocol::fresh_state_transition_matches(260, 260, 11, 10));
    printer_protocol::acknowledgement_tracker acks;
    const auto before = acks.version();
    acks.observe(12);
    assert(acks.observed_after(12, before));
    const auto current = acks.version();
    assert(!acks.observed_after(12, current));
    const auto before_interleaved = acks.version();
    acks.observe(13);
    acks.observe(0);
    assert(acks.observed_after(13, before_interleaved));
    printer_protocol::consecutive_sample_gate temperature_gate(2, 100);
    assert(!temperature_gate.update(100, true));
    assert(!temperature_gate.update(101, true));
    assert(!temperature_gate.update(101, true));
    assert(temperature_gate.update(102, true));

    motor_interlock::arbiter arbiter;
    assert(arbiter.try_claim(motor_interlock::owner::unload));
    assert(!arbiter.try_claim(motor_interlock::owner::load));
    arbiter.request_stop();
    assert(arbiter.stop_requested());
    arbiter.release(motor_interlock::owner::unload);

    motor_interlock::falling_edge_debouncer debounce(5);
    for (int i = 0; i < 20; ++i) assert(!debounce.update(true));
    for (int i = 0; i < 5; ++i) assert(!debounce.update(false));
    for (int i = 0; i < 4; ++i) assert(!debounce.update(true));
    assert(debounce.update(true));
    for (int i = 0; i < 10; ++i) assert(!debounce.update(true));
    for (int i = 0; i < 5; ++i) assert(!debounce.update(false));
    for (int i = 0; i < 4; ++i) assert(!debounce.update(true));
    assert(debounce.update(true));

    filament_flow_policy::resume_evidence evidence{
        true, true, true, true, true, 2, 2
    };
    assert(filament_flow_policy::can_resume(evidence));
    evidence.mqtt_fresh = false;
    assert(!filament_flow_policy::can_resume(evidence));
    assert(filament_flow_policy::channel_after_attempt(2, 3, true, false) == 0);
    assert(filament_flow_policy::channel_after_attempt(2, 3, true, true) == 3);
    assert(filament_flow_policy::confirmed_startup_channel(1, false, 4) == 0);
    assert(filament_flow_policy::confirmed_startup_channel(5, true, 4) == 0);
    assert(filament_flow_policy::confirmed_startup_channel(3, true, 4) == 3);

    filament_change::trigger_state trigger;
    trigger.update_bed_target(9);
    trigger.update_gcode_state("PAUSE");
    assert(trigger.request(8).kind == filament_change::trigger_kind::initial_load);
    assert(trigger.request(8).channel == 1);

    std::cout << "printer flow host tests passed\n";
    return 0;
}
