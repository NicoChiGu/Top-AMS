#include "filament_change_trigger.hpp"
#include "unity.h"

using filament_change::trigger_readiness;
using filament_change::trigger_state;

TEST_CASE("filament trigger accepts fields in one MQTT report", "[filament][trigger]") {
    trigger_state state;
    state.update_bed_target(1);
    state.update_gcode_state("PAUSE");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::ready),
                          static_cast<int>(state.evaluate(8)));
}

TEST_CASE("filament trigger merges channel before pause", "[filament][trigger]") {
    trigger_state state;
    state.update_bed_target(2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::waiting_for_pause),
                          static_cast<int>(state.evaluate(8)));
    state.update_gcode_state("PAUSE");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::ready),
                          static_cast<int>(state.evaluate(8)));
}

TEST_CASE("filament trigger merges pause before channel", "[filament][trigger]") {
    trigger_state state;
    state.update_gcode_state("PAUSE");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::waiting_for_channel),
                          static_cast<int>(state.evaluate(8)));
    state.update_bed_target(2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::ready),
                          static_cast<int>(state.evaluate(8)));
}

TEST_CASE("filament trigger consumes one pause only once", "[filament][trigger]") {
    trigger_state state;
    state.update_bed_target(2);
    state.update_gcode_state("PAUSE");
    state.consume();
    state.update_bed_target(2);
    state.update_gcode_state("PAUSE");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::consumed),
                          static_cast<int>(state.evaluate(8)));

    state.update_gcode_state("RUNNING");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::waiting_for_pause),
                          static_cast<int>(state.evaluate(8)));
    state.update_bed_target(1);
    state.update_gcode_state("PAUSE");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::ready),
                          static_cast<int>(state.evaluate(8)));
}

TEST_CASE("filament trigger rejects invalid and ordinary temperatures", "[filament][trigger]") {
    trigger_state state;
    state.update_gcode_state("PAUSE");
    state.update_bed_target(9);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::invalid_channel),
                          static_cast<int>(state.evaluate(8)));

    state.update_bed_target(60);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::waiting_for_channel),
                          static_cast<int>(state.evaluate(8)));
    state.update_gcode_state("RUNNING");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::idle),
                          static_cast<int>(state.evaluate(8)));
}

TEST_CASE("filament trigger remains pending until caller consumes it", "[filament][trigger]") {
    trigger_state state;
    state.update_bed_target(1);
    state.update_gcode_state("PAUSE");
    // A busy caller deliberately leaves the candidate unconsumed.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::ready),
                          static_cast<int>(state.evaluate(8)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::ready),
                          static_cast<int>(state.evaluate(8)));
}

TEST_CASE("filament trigger reset drops cached reconnect state", "[filament][trigger]") {
    trigger_state state;
    state.update_bed_target(1);
    state.update_gcode_state("PAUSE");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::ready),
                          static_cast<int>(state.evaluate(8)));

    state.reset();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trigger_readiness::idle),
                          static_cast<int>(state.evaluate(8)));
    TEST_ASSERT_EQUAL_INT(-1, state.bed_target());
}
