#!/usr/bin/env python3
"""Source-level guards for externally visible OmiGlass wire behavior.

These checks are intentionally small and explicit. They do not prove runtime BLE
behavior; they prevent accidental source drift in byte-level protocol invariants
that were audited during Phase 11.
"""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
BLE = (ROOT / "main" / "ble_service.cpp").read_text(encoding="utf-8")
AUDIO = (ROOT / "main" / "audio_tx_service.cpp").read_text(encoding="utf-8")
PHOTO = (ROOT / "main" / "photo_service.cpp").read_text(encoding="utf-8")
CONFIG = (ROOT / "main" / "omiglass_config.h").read_text(encoding="utf-8")

CHECKS = {
    # OTA Control is a historical compatibility quirk: app.cpp returned [status, 0].
    "OTA Control READ keeps zero progress byte":
        "const uint8_t value[2] = {s_ota_status.load(), 0};",
    "OTA Data status keeps real progress byte":
        "const uint8_t value[2] = {s_ota_status.load(), s_ota_progress.load()};",
    "OTA notify sends status and progress":
        "const uint8_t value[2] = {status, progress};",
    # Audio packet header: packet index LE + sub-index 0.
    "Audio packet index low byte":
        "packet[0] = static_cast<uint8_t>(s_packet_index & 0xFF);",
    "Audio packet index high byte":
        "packet[1] = static_cast<uint8_t>((s_packet_index >> 8) & 0xFF);",
    "Audio packet sub-index":
        "packet[2] = OMI_AUDIO_SUB_INDEX;",
    # Photo packet: first packet orientation, LE chunk index, FF FF terminator.
    "Photo first packet orientation":
        "packet[2] = s_frame.orientation;",
    "Photo chunk low byte":
        "packet[0] = static_cast<uint8_t>(s_chunk_index & 0xFF);",
    "Photo chunk high byte":
        "packet[1] = static_cast<uint8_t>((s_chunk_index >> 8) & 0xFF);",
    "Photo FF FF terminator":
        "const uint8_t end_marker[2] = {0xFF, 0xFF};",
    # Config-side sizes are part of the protocol contract.
    "Audio header size 3":
        "OMI_AUDIO_PACKET_HEADER_SIZE = 3",
    "Audio sub-index zero":
        "OMI_AUDIO_SUB_INDEX = 0",
    "Photo first JPEG bytes 199":
        "OMI_PHOTO_FIRST_CHUNK_JPEG_BYTES = 199",
    "Photo other JPEG bytes 200":
        "OMI_PHOTO_OTHER_CHUNK_JPEG_BYTES = 200",
    "Photo packet max 202":
        "OMI_PHOTO_PACKET_MAX_BYTES = 202",
}

SOURCES = BLE + "\n" + AUDIO + "\n" + PHOTO + "\n" + CONFIG

failed = False
for description, token in CHECKS.items():
    if token not in SOURCES:
        print(f"[FAIL] {description}: missing {token!r}", file=sys.stderr)
        failed = True
    else:
        print(f"[PASS] {description}")

if "case CharacteristicId::OTA_CONTROL:\n        return append_ota_control_status(ctxt->om);" not in BLE:
    print("[FAIL] OTA Control is not routed to compatibility read handler", file=sys.stderr)
    failed = True
else:
    print("[PASS] OTA Control routes to compatibility read handler")

if "case CharacteristicId::OTA_DATA:\n        return append_ota_status(ctxt->om);" not in BLE:
    print("[FAIL] OTA Data is not routed to progress read handler", file=sys.stderr)
    failed = True
else:
    print("[PASS] OTA Data routes to progress read handler")

if failed:
    raise SystemExit(1)

print("\nPhase 11 wire behavior checks passed.")
