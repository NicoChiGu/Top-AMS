# Verification Record

Date: 2026-07-21

## Completed in this workspace

- OpenSpec static structure: 3 requirements and 9 correctly headed scenarios.
- Web resource generation: generated 4, 6 and 8 channel variants without an unresolved channel placeholder.
- JavaScript syntax: parsed the generated page's inline script with Node.js.
- Browser QA: checked the System Diagnostics page at desktop and 390 px mobile widths; exercised level/module filters and pause/continue-to-latest controls; no page warnings or errors were reported for the generated page.
- Source hygiene: `git diff --check` passed.
- Sensitive-output scan: no active diagnostic/serial statement emits the MQTT password, device serial number, raw MQTT payload or parsed full JSON body.
- Host diagnostic tests: compiled with MSVC C++20 and passed ring boundary/overflow/clear, UTF-8 truncation, concurrent read/write, MQTT stale/recovery/error and timeline timeout-retention cases. The host layout measured 152 bytes per event and 9,728 bytes for the 64-entry ring.
- Target-side tests added under `main/test`: empty/full/overflow ring behavior, monotonic sequence after clear, 112-byte UTF-8 boundary, concurrent writers, MQTT reconnect/error metrics and timeout retention in the filament timeline.

## Environment limits

- `openspec` is not installed, so `openspec validate add-runtime-diagnostics --strict` could not be run. The change remains pending strict CLI validation.
- ESP-IDF, CMake, Ninja and a C++ compiler are not installed, so firmware and Unity test builds could not be run locally.
- No ESP32-C3 or printer was attached, so hardware-in-the-loop checks remain pending: real MQTT reconnect/stale recovery, one full automatic filament change, simultaneous browser client count, and long-running heap trend.

## Required release gates

1. Run `openspec validate add-runtime-diagnostics --strict`.
2. Build `MOTORS_4`, `MOTORS_6` and `MOTORS_8` with ESP-IDF 5.3.2.
3. Build and run the `main/test` Unity cases on ESP32-C3.
4. Confirm the enabled diagnostics reduce startup free heap by no more than 12 KiB and do not cause a continuing minimum-heap decline.
5. Perform the hardware-in-the-loop scenarios listed above before release.
