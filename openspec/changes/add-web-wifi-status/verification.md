# Verification

## Local verification results (2026-07-20)
- Passed the Web runtime harness for RSSI thresholds at `-55`, `-56`, `-67`, `-68`, `-75`, and `-76 dBm`; verified the expected four signal levels and active bar counts.
- Passed Web runtime checks for a special-character SSID, IP and raw dBm display, the 5-second request action, stale-value clearing, and WebSocket-close fallback.
- Generated embedded Web assets successfully for 4, 6, and 8 channels; each output replaced the channel placeholder and contained one Wi-Fi status component.
- Passed static firmware checks for the four read-only fields, client-scoped replies, initial connect snapshot, `get_wifi_status` action, invalid-IP handling, and disconnected RSSI `null` behavior.
- Confirmed the pinned ESPAsyncWebServer 3.7.4 API provides `AsyncWebSocketClient::text(const String&)`, matching the client-scoped response implementation.
- Inspected the desktop/mobile status-bar rules for constrained SSID width, secondary-data wrapping, and separate MQTT alignment. In-app visual rendering was attempted but blocked by the browser URL security policy for the local in-memory fixture.
- Passed `git diff --check` and a static OpenSpec format check. The current environment does not provide the OpenSpec CLI, ESP-IDF, CMake, Ninja, or a C++ compiler, so strict CLI validation and a firmware build remain environment-dependent checks.

## Automated and static checks
- Run the inline page script in a mocked DOM/WebSocket harness and verify RSSI thresholds, text-only SSID insertion, timer lifecycle, request payload, and disconnected states.
- Run `convert_html_to_hpp.py` with channel counts 4, 6, and 8 and confirm no channel placeholder remains.
- Run `openspec validate add-web-wifi-status --strict` when the OpenSpec CLI is available.
- Build the firmware for `MOTORS_4`, `MOTORS_6`, and `MOTORS_8` when the ESP-IDF toolchain is available.

## Hardware-in-the-loop acceptance
- On WebSocket connect, the page shows the access point's actual SSID, the ESP32 station IP, and an RSSI value matching the device API.
- Moving the device relative to the access point updates the dBm value and crosses the documented signal thresholds without reloading the page.
- A long or special-character SSID is displayed as text, is safely truncated on a narrow screen, and remains available through its title text.
- Losing Wi-Fi or the WebSocket marks the information unavailable and does not retain stale SSID, IP, or RSSI values as current.
- Restoring connectivity and reloading the page restores the current network information without changing Wi-Fi, MQTT, motor, or channel configuration.
