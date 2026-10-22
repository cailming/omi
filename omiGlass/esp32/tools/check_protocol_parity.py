#!/usr/bin/env python3
"""Phase 11：检查 Arduino 基线与 Native ESP-IDF 的关键协议常量是否仍然一致。"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
ARDUINO_CONFIG = ROOT / "omiGlass/firmware/src/config.h"
ARDUINO_APP = ROOT / "omiGlass/firmware/src/app.cpp"
IDF_CONFIG = ROOT / "omiGlass/esp32/main/omiglass_config.h"
IDF_BLE = ROOT / "omiGlass/esp32/main/ble_service.cpp"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def parse_define(text: str, name: str) -> str:
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(.+?)\s*(?://.*)?$", text, re.MULTILINE)
    if not match:
        raise ValueError(f"找不到 Arduino 宏 {name}")
    return normalize(match.group(1))


def parse_current(text: str, name: str) -> str:
    define = re.search(rf"^#define\s+{re.escape(name)}\s+(.+?)\s*(?://.*)?$", text, re.MULTILINE)
    if define:
        return normalize(define.group(1))

    constexpr = re.search(
        rf"^constexpr\s+[^;=]+\s+{re.escape(name)}\s*=\s*(.+?);\s*(?://.*)?$",
        text,
        re.MULTILINE,
    )
    if not constexpr:
        raise ValueError(f"找不到 ESP-IDF 常量 {name}")
    return normalize(constexpr.group(1))


def normalize(value: str) -> str:
    value = value.strip()
    # 去掉 C/C++ 数值后缀，保留字符串和十六进制值的语义。
    value = re.sub(r"(?<=\d)[uUlLfF]+\b", "", value)
    return value


def parse_int(value: str) -> int:
    value = normalize(value)
    value = value.strip("() ")
    return int(value, 0)


def parse_uuid_from_nimble(text: str, symbol: str) -> str:
    pattern = (
        rf"static\s+const\s+ble_uuid128_t\s+{re.escape(symbol)}\s*=\s*"
        rf"BLE_UUID128_INIT\((.*?)\);"
    )
    match = re.search(pattern, text, re.DOTALL)
    if not match:
        raise ValueError(f"找不到 NimBLE UUID {symbol}")

    values = [int(item, 16) for item in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(1))]
    if len(values) != 16:
        raise ValueError(f"{symbol} 应包含 16 个 UUID 字节，实际为 {len(values)}")

    canonical = bytes(reversed(values)).hex().upper()
    return f"{canonical[0:8]}-{canonical[8:12]}-{canonical[12:16]}-{canonical[16:20]}-{canonical[20:32]}"


def strip_quotes(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        return value[1:-1]
    return value


def main() -> int:
    arduino_config = read(ARDUINO_CONFIG)
    arduino_app = read(ARDUINO_APP)
    idf_config = read(IDF_CONFIG)
    idf_ble = read(IDF_BLE)

    errors: list[str] = []

    # 名称不同，但语义必须一致的基础/音频/BLE/电池/OTA 常量。
    mapped_values = {
        "BLE_DEVICE_NAME": "OMI_BLE_DEVICE_NAME",
        "FIRMWARE_VERSION_STRING": "OMI_FIRMWARE_VERSION_STRING",
        "HARDWARE_REVISION": "OMI_HARDWARE_REVISION",
        "MANUFACTURER_NAME": "OMI_MANUFACTURER_NAME",
        "MIC_SAMPLE_RATE": "OMI_MIC_SAMPLE_RATE",
        "MIC_BUFFER_SAMPLES": "OMI_MIC_BUFFER_SAMPLES",
        "MIC_GAIN": "OMI_MIC_GAIN",
        "AUDIO_RING_BUFFER_SAMPLES": "OMI_AUDIO_RING_BUFFER_SAMPLES",
        "AUDIO_CODEC_ID": "OMI_AUDIO_CODEC_ID",
        "OPUS_FRAME_SAMPLES": "OMI_OPUS_FRAME_SAMPLES",
        "OPUS_OUTPUT_MAX_BYTES": "OMI_OPUS_OUTPUT_MAX_BYTES",
        "OPUS_BITRATE": "OMI_OPUS_BITRATE",
        "OPUS_COMPLEXITY": "OMI_OPUS_COMPLEXITY",
        "OPUS_VBR": "OMI_OPUS_VBR",
        "AUDIO_PACKET_HEADER_SIZE": "OMI_AUDIO_PACKET_HEADER_SIZE",
        "AUDIO_TX_RING_BUFFER_SIZE": "OMI_AUDIO_TX_QUEUE_FRAMES",
        "BLE_MTU_SIZE": "OMI_BLE_MTU_SIZE",
        "BLE_ADV_MIN_INTERVAL": "OMI_BLE_ADV_MIN_INTERVAL",
        "BLE_ADV_MAX_INTERVAL": "OMI_BLE_ADV_MAX_INTERVAL",
        "BLE_CONN_MIN_INTERVAL": "OMI_BLE_CONN_MIN_INTERVAL",
        "BLE_CONN_MAX_INTERVAL": "OMI_BLE_CONN_MAX_INTERVAL",
        "BLE_CONN_LATENCY": "OMI_BLE_CONN_LATENCY",
        "BLE_CONN_TIMEOUT": "OMI_BLE_CONN_TIMEOUT",
        "PHOTO_CAPTURE_INTERVAL_MS": "OMI_CAMERA_CAPTURE_INTERVAL_MS",
        "CAMERA_JPEG_QUALITY": "OMI_CAMERA_JPEG_QUALITY",
        "CAMERA_XCLK_FREQ": "OMI_CAMERA_XCLK_HZ",
        "VOLTAGE_DIVIDER_RATIO": "OMI_BATTERY_VOLTAGE_DIVIDER_RATIO",
        "BATTERY_MAX_VOLTAGE": "OMI_BATTERY_MAX_VOLTAGE",
        "BATTERY_MIN_VOLTAGE": "OMI_BATTERY_MIN_VOLTAGE",
        "BATTERY_CRITICAL_VOLTAGE": "OMI_BATTERY_CRITICAL_VOLTAGE",
        "BATTERY_LOW_VOLTAGE": "OMI_BATTERY_LOW_VOLTAGE",
        "BATTERY_TASK_INTERVAL_MS": "OMI_BATTERY_CHECK_INTERVAL_MS",
        "BATTERY_REPORT_INTERVAL_MS": "OMI_BATTERY_REPORT_INTERVAL_MS",
        "WIFI_CONNECT_TIMEOUT_MS": "OMI_WIFI_CONNECT_TIMEOUT_MS",
        "WIFI_MAX_SSID_LEN": "OMI_WIFI_MAX_SSID_LEN",
        "WIFI_MAX_PASS_LEN": "OMI_WIFI_MAX_PASSWORD_LEN",
        "OTA_MAX_URL_LEN": "OMI_OTA_MAX_URL_LEN",
    }

    for old_name, new_name in mapped_values.items():
        old_value = parse_define(arduino_config, old_name)
        new_value = parse_current(idf_config, new_name)
        if old_value != new_value:
            errors.append(f"{old_name}={old_value} != {new_name}={new_value}")

    command_status_names = [
        "OTA_CMD_SET_WIFI",
        "OTA_CMD_START_OTA",
        "OTA_CMD_CANCEL_OTA",
        "OTA_CMD_GET_STATUS",
        "OTA_CMD_SET_URL",
        "OTA_STATUS_IDLE",
        "OTA_STATUS_WIFI_CONNECTING",
        "OTA_STATUS_WIFI_CONNECTED",
        "OTA_STATUS_WIFI_FAILED",
        "OTA_STATUS_DOWNLOADING",
        "OTA_STATUS_DOWNLOAD_COMPLETE",
        "OTA_STATUS_DOWNLOAD_FAILED",
        "OTA_STATUS_INSTALLING",
        "OTA_STATUS_INSTALL_COMPLETE",
        "OTA_STATUS_INSTALL_FAILED",
        "OTA_STATUS_REBOOTING",
        "OTA_STATUS_ERROR",
    ]
    for old_name in command_status_names:
        new_name = f"OMI_{old_name}"
        old_value = parse_int(parse_define(arduino_config, old_name))
        new_value = parse_int(parse_current(idf_config, new_name))
        if old_value != new_value:
            errors.append(f"{old_name}=0x{old_value:02X} != {new_name}=0x{new_value:02X}")

    uuid_symbols = {
        "OMI_SERVICE_UUID": "OMI_SERVICE_UUID",
        "AUDIO_DATA_UUID": "AUDIO_DATA_UUID",
        "AUDIO_CODEC_UUID": "AUDIO_CODEC_UUID",
        "PHOTO_DATA_UUID": "PHOTO_DATA_UUID",
        "PHOTO_CONTROL_UUID": "PHOTO_CONTROL_UUID",
        "OTA_SERVICE_UUID": "OTA_SERVICE_UUID",
        "OTA_CONTROL_UUID": "OTA_CONTROL_UUID",
        "OTA_DATA_UUID": "OTA_DATA_UUID",
    }
    for old_name, current_symbol in uuid_symbols.items():
        expected = strip_quotes(parse_define(arduino_config, old_name)).upper()
        actual = parse_uuid_from_nimble(idf_ble, current_symbol)
        if expected != actual:
            errors.append(f"{old_name}={expected} != {current_symbol}={actual}")

    # 图片线协议来自 Arduino app.cpp 的实际发送逻辑，不只依赖 config.h。
    fixed_contract = {
        "OMI_AUDIO_SUB_INDEX": 0,
        "OMI_PHOTO_FIRST_CHUNK_HEADER_BYTES": 3,
        "OMI_PHOTO_OTHER_CHUNK_HEADER_BYTES": 2,
        "OMI_PHOTO_FIRST_CHUNK_JPEG_BYTES": 199,
        "OMI_PHOTO_OTHER_CHUNK_JPEG_BYTES": 200,
        "OMI_PHOTO_PACKET_MAX_BYTES": 202,
        "OMI_PHOTO_END_MARKER": 0xFFFF,
        "OMI_CAMERA_FIXED_ORIENTATION": 2,
    }
    for name, expected in fixed_contract.items():
        actual = parse_int(parse_current(idf_config, name))
        if actual != expected:
            errors.append(f"{name}={actual}，协议要求={expected}")

    # 防止基线 app.cpp 被替换后，脚本还在用过时的硬编码契约。
    required_arduino_fragments = [
        "audio_packet_buffer[2] = 0",
        "bytes_to_copy = (remaining > 199) ? 199 : remaining",
        "bytes_to_copy = (remaining > 200) ? 200 : remaining",
        "s_compressed_frame_2[0] = 0xFF",
        "s_compressed_frame_2[1] = 0xFF",
    ]
    for fragment in required_arduino_fragments:
        if fragment not in arduino_app:
            errors.append(f"Arduino 基线图片/音频协议实现发生变化，缺少片段：{fragment}")

    if errors:
        print("Phase 11 协议一致性检查失败：", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print("Phase 11 协议一致性检查通过：UUID、Codec、Audio/Photo 包格式、BLE/OTA 关键常量均与 Arduino 基线一致。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
