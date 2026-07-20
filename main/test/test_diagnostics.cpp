#define NO_DEBUG

#include <atomic>
#include <array>
#include <cstring>
#include <string>
#include <thread>

#include "diagnostics.hpp"
#include "unity.h"

TEST_CASE("diagnostics ring handles empty and full states", "[diagnostics][ring]") {
    diagnostics::log_store store;

    auto batch = store.read_batch(0);
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(batch.count));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(batch.total_count));

    for (uint32_t i = 0; i < diagnostics::log_capacity; ++i)
        store.append(diagnostics::level::info, diagnostics::log_module::system, "TEST", std::to_string(i));

    const auto stats = store.stats();
    TEST_ASSERT_EQUAL_UINT32(64, static_cast<uint32_t>(stats.count));
    TEST_ASSERT_EQUAL_UINT32(0, stats.overwritten_count);
    TEST_ASSERT_EQUAL_UINT32(1, store.read_batch(0).events[0].seq);

    batch = store.read_batch(48);
    TEST_ASSERT_EQUAL_UINT32(16, static_cast<uint32_t>(batch.count));
    TEST_ASSERT_EQUAL_UINT32(64, batch.events[15].seq);
}

TEST_CASE("diagnostics ring overwrites oldest event in sequence", "[diagnostics][ring]") {
    diagnostics::log_store store;
    for (uint32_t i = 0; i < diagnostics::log_capacity + 1; ++i)
        store.append(diagnostics::level::warning, diagnostics::log_module::mqtt, "OVERWRITE", std::to_string(i));

    const auto first = store.read_batch(0);
    const auto last = store.read_batch(48);
    TEST_ASSERT_EQUAL_UINT32(64, static_cast<uint32_t>(first.total_count));
    TEST_ASSERT_EQUAL_UINT32(1, first.overwritten_count);
    TEST_ASSERT_EQUAL_UINT32(2, first.events[0].seq);
    TEST_ASSERT_EQUAL_UINT32(65, last.events[15].seq);
}

TEST_CASE("clearing diagnostics retains the monotonic sequence", "[diagnostics][ring]") {
    diagnostics::log_store store;
    store.append(diagnostics::level::info, diagnostics::log_module::web, "BEFORE_CLEAR", "before");
    const uint32_t cleared_through = store.clear();

    const auto value = store.append(diagnostics::level::info, diagnostics::log_module::web,
                                    "AFTER_CLEAR", "after");
    const auto stats = store.stats();
    TEST_ASSERT_EQUAL_UINT32(2, value.seq);
    TEST_ASSERT_EQUAL_UINT32(1, cleared_through);
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(stats.count));
    TEST_ASSERT_EQUAL_UINT32(0, stats.overwritten_count);
}

TEST_CASE("diagnostics preserves UTF-8 boundaries at 112 bytes", "[diagnostics][utf8]") {
    diagnostics::log_store store;
    const std::string exact = std::string(109, 'a') + "\xE6\x96\x99";
    const auto exact_value = store.append(diagnostics::level::info, diagnostics::log_module::filament,
                                          "UTF8_EXACT", exact);
    TEST_ASSERT_EQUAL_UINT32(112, static_cast<uint32_t>(std::strlen(exact_value.message)));
    TEST_ASSERT_EQUAL_MEMORY("\xE6\x96\x99", exact_value.message + 109, 3);

    const std::string split = std::string(111, 'b') + "\xE6\x96\x99";
    const auto split_value = store.append(diagnostics::level::info, diagnostics::log_module::filament,
                                          "UTF8_SPLIT", split);
    TEST_ASSERT_EQUAL_UINT32(111, static_cast<uint32_t>(std::strlen(split_value.message)));
    TEST_ASSERT_EQUAL_CHAR('\0', split_value.message[111]);
}

TEST_CASE("diagnostics ring serializes concurrent writers", "[diagnostics][ring][concurrency]") {
    diagnostics::log_store store;
    std::atomic<bool> writers_running{true};
    std::atomic<bool> reader_observed_order{true};
    std::thread reader([&store, &writers_running, &reader_observed_order]() {
        while (writers_running.load()) {
            const auto batch = store.read_batch(0);
            for (size_t i = 1; i < batch.count; ++i) {
                if (batch.events[i - 1].seq >= batch.events[i].seq)
                    reader_observed_order = false;
            }
            std::this_thread::yield();
        }
    });
    std::array<std::thread, 4> writers;
    for (size_t writer = 0; writer < writers.size(); ++writer) {
        writers[writer] = std::thread([&store, writer]() {
            for (size_t i = 0; i < 100; ++i) {
                store.append(diagnostics::level::debug, diagnostics::log_module::motor,
                             "CONCURRENT", std::to_string(writer * 100 + i));
            }
        });
    }
    for (auto& writer : writers)
        writer.join();
    writers_running = false;
    reader.join();

    const auto stats = store.stats();
    const auto first = store.read_batch(0);
    const auto last = store.read_batch(48);
    TEST_ASSERT_EQUAL_UINT32(64, static_cast<uint32_t>(stats.count));
    TEST_ASSERT_EQUAL_UINT32(336, stats.overwritten_count);
    TEST_ASSERT_EQUAL_UINT32(337, first.events[0].seq);
    TEST_ASSERT_EQUAL_UINT32(400, last.events[15].seq);
    TEST_ASSERT_TRUE(reader_observed_order.load());
}

TEST_CASE("MQTT diagnostics retain reconnects and structured errors", "[diagnostics][mqtt]") {
    diagnostics::mqtt_metrics_store metrics;
    TEST_ASSERT_EQUAL_STRING("not_configured",
                             diagnostics::to_string(metrics.snapshot().transport_state));

    metrics.mark_connecting();
    metrics.mark_connected();
    metrics.mark_disconnected();
    metrics.mark_connecting();
    metrics.mark_connected();
    metrics.mark_error("tcp_transport", -1, -2, 104, "transport failed");

    const auto snapshot = metrics.snapshot();
    TEST_ASSERT_EQUAL_STRING("error", diagnostics::to_string(snapshot.transport_state));
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.reconnect_count);
    TEST_ASSERT_TRUE(snapshot.last_error.present);
    TEST_ASSERT_EQUAL_STRING("tcp_transport", snapshot.last_error.kind);
    TEST_ASSERT_EQUAL_INT32(-1, snapshot.last_error.code);
    TEST_ASSERT_EQUAL_INT32(-2, snapshot.last_error.detail_code);
    TEST_ASSERT_EQUAL_INT32(104, snapshot.last_error.socket_errno);
}

TEST_CASE("filament timeline marks later stages unreachable and retains timeouts",
          "[diagnostics][timeline]") {
    diagnostics::timeline_store timeline;
    timeline.begin(1, 2, "triggered");
    timeline.finish_stage(diagnostics::filament_stage::trigger,
                          diagnostics::stage_state::success, "accepted");
    timeline.start(diagnostics::filament_stage::unload, "unloading");
    timeline.fail(diagnostics::filament_stage::unload, "unload timeout", true);

    auto snapshot = timeline.snapshot();
    TEST_ASSERT_EQUAL_STRING("error", diagnostics::to_string(snapshot.state));
    TEST_ASSERT_EQUAL_STRING("error", diagnostics::to_string(
        snapshot.stages[diagnostics::stage_index(diagnostics::filament_stage::unload)].state));
    TEST_ASSERT_EQUAL_STRING("not_reached", diagnostics::to_string(
        snapshot.stages[diagnostics::stage_index(diagnostics::filament_stage::heat)].state));
    TEST_ASSERT_TRUE(snapshot.last_timeout.present);
    TEST_ASSERT_EQUAL_STRING("unload", diagnostics::to_string(snapshot.last_timeout.stage));

    timeline.begin(2, 3, "next run");
    snapshot = timeline.snapshot();
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.run_id);
    TEST_ASSERT_TRUE(snapshot.last_timeout.present);
    TEST_ASSERT_EQUAL_STRING("unload", diagnostics::to_string(snapshot.last_timeout.stage));
}
