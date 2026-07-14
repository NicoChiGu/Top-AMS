# Verification

## Local verification results (2026-07-14)
- Passed a static OpenSpec format check: 3 requirements, 8 correctly formatted scenarios, and no invalid scenario headings.
- Passed JavaScript syntax compilation for the embedded page script.
- Generated Web assets successfully for channel counts 4, 6, and 8 with no remaining channel placeholder.
- Passed a lightweight Web runtime harness for 4, 6, and 8 channels: row generation, direction-state synchronization, boolean update payload, and busy-state rejection.
- Passed static firmware checks for the four-case direction mapping, persistent default, logical timing selection, and firmware-side busy guard.
- Responsive desktop/mobile DOM and CSS rules were inspected. In-app visual rendering was attempted but blocked by the browser URL security policy for the local in-memory fixture.
- The current environment does not provide the OpenSpec CLI, ESP-IDF, CMake, Ninja, or a C++ compiler, so strict CLI validation and firmware builds remain environment-dependent checks.

## Automated and static checks
- OpenSpec delta structure and scenario headings are checked locally; run `openspec validate add-per-channel-motor-reversal --strict` when the OpenSpec CLI is available.
- Generate Web assets for channel counts 4, 6, and 8 and verify the generated direction-control count.
- Build the firmware for `MOTORS_4`, `MOTORS_6`, and `MOTORS_8` when the ESP-IDF toolchain is available.

## Hardware-in-the-loop acceptance
- With reversal disabled, logical forward drives `forward` and logical backward drives `backward`.
- With reversal enabled for one channel, logical forward drives `backward` and logical backward drives `forward`.
- Manual forward/backward, loading, automatic filament change, and assist feeding use the same mapping while retaining their logical timing.
- The active GPIO returns low after the action, and other channels retain their configured direction.
- The saved reversal setting survives a power cycle and is restored on WebSocket reconnect.
- Direction changes are disabled and rejected while locked, changing, loading, or running any motor.
