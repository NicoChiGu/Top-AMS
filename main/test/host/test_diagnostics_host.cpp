#define NO_DEBUG

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "diagnostics.hpp"

using namespace std::chrono_literals;

namespace {

void test_ring_boundaries() {
    diagnostics::log_store store;
    assert(store.read_batch(0).count == 0);

    for (uint32_t i = 0; i < diagnostics::log_capacity; ++i)
        store.append(diagnostics::level::info, diagnostics::log_module::system, "TEST", std::to_string(i));

    assert(store.stats().count == 64);
    assert(store.stats().overwritten_count == 0);
    assert(store.read_batch(0).events[0].seq == 1);
    assert(store.read_batch(48).events[15].seq == 64);

    store.append(diagnostics::level::warning, diagnostics::log_module::mqtt, "OVERWRITE", "65");
    assert(store.stats().count == 64);
    assert(store.stats().overwritten_count == 1);
    assert(store.read_batch(0).events[0].seq == 2);
    assert(store.read_batch(48).events[15].seq == 65);

    const uint32_t cleared_through = store.clear();
    const auto after_clear = store.append(diagnostics::level::info, diagnostics::log_module::web,
                                          "AFTER_CLEAR", "after");
    assert(cleared_through == 65);
    assert(after_clear.seq == 66);
    assert(store.stats().count == 1);
    assert(store.stats().overwritten_count == 0);
}

void test_utf8_boundary() {
    diagnostics::log_store store;
    const std::string exact = std::string(109, 'a') + "\xE6\x96\x99";
    const auto exact_value = store.append(diagnostics::level::info, diagnostics::log_module::filament,
                                          "UTF8_EXACT", exact);
    assert(std::strlen(exact_value.message) == 112);
    assert(std::memcmp(exact_value.message + 109, "\xE6\x96\x99", 3) == 0);

    const std::string split = std::string(111, 'b') + "\xE6\x96\x99";
    const auto split_value = store.append(diagnostics::level::info, diagnostics::log_module::filament,
                                          "UTF8_SPLIT", split);
    assert(std::strlen(split_value.message) == 111);
    assert(split_value.message[111] == '\0');
}

void test_concurrent_readers_and_writers() {
    diagnostics::log_store store;
    std::atomic<bool> writers_running{true};
    std::atomic<bool> ordered{true};
    std::thread reader([&]() {
        while (writers_running.load()) {
            const auto batch = store.read_batch(0);
            for (size_t i = 1; i < batch.count; ++i) {
                if (batch.events[i - 1].seq >= batch.events[i].seq)
                    ordered = false;
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

    assert(ordered.load());
    assert(store.stats().count == 64);
    assert(store.stats().overwritten_count == 336);
    assert(store.read_batch(0).events[0].seq == 337);
    assert(store.read_batch(48).events[15].seq == 400);
}

void test_mqtt_stale_and_recovery() {
    diagnostics::mqtt_metrics_store metrics;
    metrics.mark_connected();
    std::this_thread::sleep_for(5ms);
    assert(metrics.mark_stale_if_needed(1));
    assert(!metrics.mark_stale_if_needed(1));
    assert(metrics.snapshot().stale);

    metrics.mark_disconnected();
    metrics.mark_connecting();
    metrics.mark_connected();
    assert(!metrics.mark_stale_if_needed(50));
    assert(metrics.mark_received());
    assert(!metrics.snapshot().stale);
    assert(metrics.snapshot().reconnect_count == 1);

    metrics.mark_error("tcp_transport", -1, -2, 104, "transport failed");
    const auto snapshot = metrics.snapshot();
    assert(snapshot.last_error.present);
    assert(std::strcmp(snapshot.last_error.kind, "tcp_transport") == 0);
    assert(snapshot.last_error.code == -1);
    assert(snapshot.last_error.detail_code == -2);
    assert(snapshot.last_error.socket_errno == 104);
}

void test_timeline_timeout_retention() {
    diagnostics::timeline_store timeline;
    timeline.begin(1, 2, "triggered");
    timeline.finish_stage(diagnostics::filament_stage::trigger,
                          diagnostics::stage_state::success, "accepted");
    timeline.start(diagnostics::filament_stage::unload, "unloading");
    timeline.fail(diagnostics::filament_stage::unload, "unload timeout", true);

    auto snapshot = timeline.snapshot();
    assert(snapshot.state == diagnostics::run_state::error);
    assert(snapshot.stages[diagnostics::stage_index(diagnostics::filament_stage::unload)].state ==
           diagnostics::stage_state::error);
    assert(snapshot.stages[diagnostics::stage_index(diagnostics::filament_stage::heat)].state ==
           diagnostics::stage_state::not_reached);
    assert(snapshot.last_timeout.present);
    assert(snapshot.last_timeout.stage == diagnostics::filament_stage::unload);

    timeline.begin(2, 3, "next run");
    snapshot = timeline.snapshot();
    assert(snapshot.run_id == 2);
    assert(snapshot.last_timeout.present);
    assert(snapshot.last_timeout.stage == diagnostics::filament_stage::unload);
}

} // namespace

int main() {
    static_assert(sizeof(diagnostics::event) * diagnostics::log_capacity <= 10 * 1024);
    test_ring_boundaries();
    test_utf8_boundary();
    test_concurrent_readers_and_writers();
    test_mqtt_stale_and_recovery();
    test_timeline_timeout_retention();
    std::cout << "diagnostics host tests passed (event=" << sizeof(diagnostics::event)
              << " bytes, ring="
              << sizeof(diagnostics::event) * diagnostics::log_capacity << " bytes)\n";
    return 0;
}
