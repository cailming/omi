# OmiGlass Arduino -> ESP-IDF Migration Plan

Branch: `feature/omiglass-esp-idf-migration`

Reference implementation: `omiGlass/firmware/`

Target implementation: `omiGlass/esp32/`

## Migration principles

1. Keep the current Arduino firmware intact until the ESP-IDF implementation reaches functional parity.
2. Preserve BLE UUIDs, packet formats, photo chunking behavior and OTA command protocol unless explicitly documented.
3. Prefer native ESP-IDF APIs/components. Do not introduce Arduino-as-component as a migration shortcut.
4. Migrate in small stages with an explicit verification target for each stage.
5. First reach feature parity with the current sequential behavior; only then refactor into independent FreeRTOS tasks/components where useful.
6. Treat BLE timing, audio latency, image transfer and power behavior as compatibility-sensitive areas.

## Phase 0 - Project skeleton and migration tracking

Status: IN PROGRESS

Deliverables:

- Native ESP-IDF project skeleton under `omiGlass/esp32/`.
- ESP32-S3 target configuration.
- Initial `app_main()` entry point.
- Migration plan and progress log.

Verification:

- `idf.py set-target esp32s3`
- `idf.py build`
- Firmware boots and prints a migration banner.

## Phase 1 - Shared configuration and basic board services

Status: NOT STARTED

Migrate:

- Device constants and firmware metadata.
- GPIO pin mapping for XIAO ESP32S3 Sense.
- Button input.
- Status LED output.
- Time helpers replacing `millis()` / `delay()`.
- Device serial derived from ESP32 eFuse MAC.

Arduino -> ESP-IDF mapping:

- `Serial.*` -> `ESP_LOGx`
- `millis()` -> `esp_timer_get_time()`
- `delay()` -> `vTaskDelay()`
- `pinMode()` / `digitalRead()` / `digitalWrite()` -> GPIO driver
- `attachInterrupt()` -> GPIO ISR service
- `ESP.getEfuseMac()` -> ESP eFuse MAC APIs
- `ESP.restart()` -> `esp_restart()`

Verification:

- LED and button behavior matches the Arduino firmware.
- Serial/device metadata is stable.

## Phase 2 - Microphone capture

Status: NOT STARTED

Migrate:

- XIAO ESP32S3 Sense PDM microphone.
- GPIO42 PDM clock.
- GPIO41 PDM data.
- 16 kHz mono PCM capture.

Implementation target:

- ESP-IDF 5.x native PDM RX I2S driver.
- Prefer `driver/i2s_pdm.h` over keeping the legacy I2S driver unchanged.

Verification:

- Stable PCM stream at 16 kHz.
- No overrun under normal load.
- PCM amplitude and noise floor comparable with Arduino implementation.

## Phase 3 - Opus encoder

Status: NOT STARTED

Migrate:

- 16 kHz input.
- 320-sample / 20 ms frames.
- 32 kbps target bitrate.
- Complexity 3.
- VBR enabled.
- Existing encoded-frame callback semantics.

Verification:

- Output frame sizes remain within existing protocol limits.
- Mobile side can decode captured audio without protocol changes.

## Phase 4 - BLE core and services

Status: NOT STARTED

This is a high-risk migration stage.

Migrate from NimBLE-Arduino to native ESP-IDF NimBLE:

- Omi main service.
- Audio data characteristic.
- Audio codec characteristic.
- Photo data characteristic.
- Photo control characteristic.
- Battery service.
- Device information service.
- OTA service.
- CCCD subscription handling.
- Connect/disconnect events.
- Advertising and connection parameters.
- MTU negotiation and notification behavior.

Compatibility rule:

The mobile application should not need changes if the migration preserves UUIDs and payload formats.

Verification:

- Existing app discovers and connects to ESP-IDF firmware.
- UUID layout is identical.
- Audio subscription works.
- Notifications remain stable at sustained traffic.

## Phase 5 - Audio uplink over BLE

Status: NOT STARTED

Migrate:

- Encoded audio ring buffer.
- Packet index and sub-index header.
- BLE audio notification path.
- Congestion-aware pacing.

Existing packet format must remain:

- byte 0-1: packet index, little-endian
- byte 2: sub-index
- remaining bytes: Opus payload

Verification:

- Existing phone app receives and decodes live audio.
- No audible dropouts under expected connection parameters.

## Phase 6 - Camera

Status: NOT STARTED

Migrate:

- XIAO ESP32S3 Sense camera GPIO mapping.
- VGA JPEG capture.
- JPEG quality and XCLK settings.
- PSRAM framebuffer allocation.
- Current orientation metadata.

The Arduino implementation already uses `esp_camera_*`, so most camera logic can be retained with small integration changes.

Verification:

- Camera initializes reliably.
- JPEG frames are captured in PSRAM.
- Image size/quality is comparable with Arduino firmware.

## Phase 7 - Photo BLE transport

Status: NOT STARTED

Migrate:

- Photo control commands.
- Chunked photo notification transfer.
- Frame index encoding.
- Orientation byte behavior.
- End-of-frame marker behavior.
- Audio-priority scheduling while a photo is transferring.

Verification:

- Existing app can request and reconstruct photos without changes.
- Audio remains responsive during image transfer.

## Phase 8 - Battery monitoring

Status: NOT STARTED

Migrate:

- GPIO2 battery ADC input.
- Voltage divider calibration.
- Percentage smoothing/reporting.
- Standard BLE Battery Service updates.

Implementation target:

- ESP-IDF ADC oneshot driver.
- ADC calibration API where supported.

Verification:

- Reported voltage matches multimeter closely enough for the existing battery model.
- Battery service behavior matches Arduino firmware.

## Phase 9 - Power management

Status: NOT STARTED

Migrate:

- Idle/power-save state.
- Light sleep.
- Deep sleep.
- Button wake.
- CPU frequency/power-management configuration.

Verification:

- BLE reconnects correctly after relevant state transitions.
- Camera/audio paths remain stable after wake.
- Power behavior is at least as good as the Arduino baseline.

## Phase 10 - Wi-Fi and OTA

Status: NOT STARTED

Migrate:

- BLE OTA command parser.
- Wi-Fi station connection.
- HTTP(S) firmware download.
- OTA partition update.
- Progress/status notifications over BLE.
- Reboot after successful update.

Implementation target:

- `esp_wifi`
- `esp_http_client` / `esp_https_ota`
- native app-update APIs

Security improvement:

- Do not reproduce Arduino `setInsecure()` behavior.
- Use certificate validation or the ESP certificate bundle for HTTPS OTA.

Verification:

- Existing BLE OTA command format continues to work.
- Firmware can update and reboot safely.
- Failed OTA leaves the current image bootable.

## Phase 11 - Integration and parity validation

Status: NOT STARTED

Validate as a complete device:

- Boot
- Button/LED
- BLE advertising/connect
- Audio capture/Opus/BLE uplink
- Camera capture/photo transfer
- Battery reporting
- Power save/light sleep/deep sleep
- OTA

Compare Arduino and ESP-IDF behavior using the same mobile application.

## Phase 12 - Native ESP-IDF architecture cleanup

Status: NOT STARTED

Only after parity is reached:

Potential components/tasks:

- `omi_ble`
- `omi_mic`
- `omi_audio`
- `omi_camera`
- `omi_battery`
- `omi_power`
- `omi_ota`
- BLE TX scheduler with audio priority

This phase improves maintainability but must not be mixed with the initial compatibility migration.
