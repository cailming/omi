# OmiGlass ESP-IDF Migration Progress

Branch: `feature/omiglass-esp-idf-migration`

Last updated: 2026-10-20

## Overall status

- Phase 0: IN PROGRESS
- Phase 1: NOT STARTED
- Phase 2: NOT STARTED
- Phase 3: NOT STARTED
- Phase 4: NOT STARTED
- Phase 5: NOT STARTED
- Phase 6: NOT STARTED
- Phase 7: NOT STARTED
- Phase 8: NOT STARTED
- Phase 9: NOT STARTED
- Phase 10: NOT STARTED
- Phase 11: NOT STARTED
- Phase 12: NOT STARTED

## Completed in Phase 0

- Created dedicated migration branch.
- Created `omiGlass/esp32/` workspace.
- Added migration plan.
- Added migration progress log.
- Preserved `omiGlass/firmware/` as the reference implementation.

## Next actions

1. Add minimal ESP-IDF root `CMakeLists.txt`.
2. Add `main/CMakeLists.txt` and `app_main.cpp`.
3. Add initial ESP32-S3 defaults.
4. Add a safe initial partition table compatible with future OTA work.
5. Mark Phase 0 complete after repository-level structure review.

## Compatibility invariants

These are treated as fixed until explicitly changed and documented:

- Hardware target: Seeed XIAO ESP32S3 Sense.
- BLE device name: `OMI Glass`.
- Existing Omi BLE UUIDs.
- Audio codec ID: 21 (Opus).
- Audio sample rate: 16 kHz.
- Opus frame: 320 samples / 20 ms.
- Camera baseline: VGA JPEG.
- Existing phone-side packet formats.
- Arduino firmware remains untouched during migration.

## Validation notes

No claim is made yet that the ESP-IDF tree builds on hardware. Phase 0 establishes the repository structure and migration contract first. Build/hardware validation will be recorded explicitly when performed.
