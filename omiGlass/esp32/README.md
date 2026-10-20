# OmiGlass ESP-IDF firmware

This directory is the native ESP-IDF migration workspace for the existing Arduino/PlatformIO firmware in `../firmware/`.

## Goals

- Keep `omiGlass/firmware/` unchanged during migration.
- Preserve the existing mobile-app BLE protocol unless a migration step explicitly documents a protocol change.
- Migrate feature-by-feature so each stage can be validated independently.
- Prefer native ESP-IDF drivers/components over Arduino compatibility layers.
- Preserve current hardware target: Seeed Studio XIAO ESP32S3 Sense.

## Source baseline

The current Arduino firmware uses:

- Seeed XIAO ESP32S3
- Arduino framework via PlatformIO
- NimBLE-Arduino
- esp32-camera
- PDM microphone over I2S
- Opus encoding
- BLE audio/photo transport
- Battery ADC monitoring
- Wi-Fi + HTTP(S) OTA
- Light/deep sleep power management

## Build target

The native project target is ESP32-S3.

Typical first-time setup:

```bash
cd omiGlass/esp32
idf.py set-target esp32s3
idf.py build
```

At Phase 0 the project is only a minimal ESP-IDF skeleton. Hardware features are introduced incrementally according to `MIGRATION_PLAN.md`.

## Important rule

Do not delete or rewrite the Arduino implementation while migration is in progress. The Arduino code remains the functional reference implementation for protocol behavior, packet formats, GPIO mapping and device behavior.
