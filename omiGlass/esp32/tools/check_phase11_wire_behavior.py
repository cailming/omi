#!/usr/bin/env python3
"""Phase 11：保护 OmiGlass 对手机端可见的线协议行为。

这些检查只做源码级契约保护，不替代真机 BLE 验证。除 Native 实现外，
同时检查 Arduino 基线 app.cpp 中的关键字节行为，避免参考基线变化后 CI
仍继续用过时的假设判断“兼容”。
"""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
ARDUINO_APP = (ROOT.parent / "firmware" / "src" / "app.cpp").read_text(encoding="utf-8")
BLE = (ROOT / "main" / "ble_service.cpp").read_text(encoding="utf-8")
AUDIO = (ROOT / "main" / "audio_tx_service.cpp").read_text(encoding="utf-8")
PHOTO = (ROOT / "main" / "photo_service.cpp").read_text(encoding="utf-8")
OTA = (ROOT / "main" / "ota_service.cpp").read_text(encoding="utf-8")
CONFIG = (ROOT / "main" / "omiglass_config.h").read_text(encoding="utf-8")

CHECKS = {
    # OTA Control 是历史兼容行为：Arduino app.cpp 的 onRead() 返回 [status, 0]。
    "OTA Control READ 保持零进度字节":
        "const uint8_t value[2] = {s_ota_status.load(), 0};",
    "OTA Data READ 保持真实进度字节":
        "const uint8_t value[2] = {s_ota_status.load(), s_ota_progress.load()};",
    "OTA Notify 发送 status + progress":
        "const uint8_t value[2] = {status, progress};",
    "OTA HTTP User-Agent 保持旧端兼容":
        'http_config.user_agent = "ESP32-OTA/1.0";',
    # Audio：packet index little-endian + sub-index 0。
    "Audio packet index 低字节":
        "packet[0] = static_cast<uint8_t>(s_packet_index & 0xFF);",
    "Audio packet index 高字节":
        "packet[1] = static_cast<uint8_t>((s_packet_index >> 8) & 0xFF);",
    "Audio packet sub-index":
        "packet[2] = OMI_AUDIO_SUB_INDEX;",
    # Photo：首包 orientation、little-endian chunk index、FF FF 结束标记。
    "Photo 首包 orientation":
        "packet[2] = s_frame.orientation;",
    "Photo chunk 低字节":
        "packet[0] = static_cast<uint8_t>(s_chunk_index & 0xFF);",
    "Photo chunk 高字节":
        "packet[1] = static_cast<uint8_t>((s_chunk_index >> 8) & 0xFF);",
    "Photo FF FF 结束标记":
        "const uint8_t end_marker[2] = {0xFF, 0xFF};",
    # Config 中这些大小直接属于手机端协议契约。
    "Audio header size=3":
        "OMI_AUDIO_PACKET_HEADER_SIZE = 3",
    "Audio sub-index=0":
        "OMI_AUDIO_SUB_INDEX = 0",
    "Photo 首包 JPEG=199":
        "OMI_PHOTO_FIRST_CHUNK_JPEG_BYTES = 199",
    "Photo 后续 JPEG=200":
        "OMI_PHOTO_OTHER_CHUNK_JPEG_BYTES = 200",
    "Photo packet max=202":
        "OMI_PHOTO_PACKET_MAX_BYTES = 202",
}

# 这些片段来自 Arduino 的真实发送代码，而不是 config.h。
# 如果原参考实现以后变化，必须重新审计 Native 行为，不能静默沿用旧结论。
ARDUINO_BASELINE_CHECKS = {
    "Arduino Audio sub-index=0": "audio_packet_buffer[2] = 0",
    "Arduino Photo 首包最多 199 JPEG":
        "bytes_to_copy = (remaining > 199) ? 199 : remaining;",
    "Arduino Photo 后续最多 200 JPEG":
        "bytes_to_copy = (remaining > 200) ? 200 : remaining;",
    "Arduino Photo 结束低字节 FF": "s_compressed_frame_2[0] = 0xFF;",
    "Arduino Photo 结束高字节 FF": "s_compressed_frame_2[1] = 0xFF;",
}

SOURCES = BLE + "\n" + AUDIO + "\n" + PHOTO + "\n" + OTA + "\n" + CONFIG

failed = False
for description, token in CHECKS.items():
    if token not in SOURCES:
        print(f"[FAIL] {description}: 缺少 {token!r}", file=sys.stderr)
        failed = True
    else:
        print(f"[PASS] {description}")

for description, token in ARDUINO_BASELINE_CHECKS.items():
    if token not in ARDUINO_APP:
        print(
            f"[FAIL] {description}: Arduino 基线已变化，缺少 {token!r}，需要重新审计协议",
            file=sys.stderr,
        )
        failed = True
    else:
        print(f"[PASS] {description}")

if "case CharacteristicId::OTA_CONTROL:\n        return append_ota_control_status(ctxt->om);" not in BLE:
    print("[FAIL] OTA Control 未路由到兼容 READ handler", file=sys.stderr)
    failed = True
else:
    print("[PASS] OTA Control 路由到兼容 READ handler")

if "case CharacteristicId::OTA_DATA:\n        return append_ota_status(ctxt->om);" not in BLE:
    print("[FAIL] OTA Data 未路由到 progress READ handler", file=sys.stderr)
    failed = True
else:
    print("[PASS] OTA Data 路由到 progress READ handler")

if failed:
    raise SystemExit(1)

print("\nPhase 11 wire behavior checks passed.")
