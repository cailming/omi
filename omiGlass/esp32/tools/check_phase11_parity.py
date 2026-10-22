#!/usr/bin/env python3
"""Phase 11 source-level parity checks for OmiGlass.

This script deliberately compares the immutable Arduino migration baseline against
Native ESP-IDF source. It only checks values that are expected to remain equal.
Intentional behavior changes (HTTPS hardening, PM strategy, queue policy, etc.)
are documented in PHASE11_SYSTEM_PARITY.md and are not asserted here.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARDUINO_CONFIG = ROOT.parent / "firmware" / "src" / "config.h"
NATIVE_CONFIG = ROOT / "main" / "omiglass_config.h"
NATIVE_BLE = ROOT / "main" / "ble_service.cpp"
NATIVE_CAMERA = ROOT / "main" / "camera_service.cpp"


class ParityError(RuntimeError):
    pass


def read(path: Path) -> str:
    if not path.exists():
        raise ParityError(f"missing file: {path}")
    return path.read_text(encoding="utf-8")


def strip_inline_comment(value: str) -> str:
    return value.split("//", 1)[0].strip()


def macro(text: str, name: str, source: str = "macro") -> str:
    match = re.search(rf"^\s*#define\s+{re.escape(name)}\s+(.+?)\s*$", text, re.MULTILINE)
    if not match:
        raise ParityError(f"{source} not found: {name}")
    return strip_inline_comment(match.group(1)).strip()


def constexpr(text: str, name: str) -> str:
    match = re.search(
        rf"constexpr\s+[^;=]+?\b{re.escape(name)}\s*=\s*([^;]+);",
        text,
        re.MULTILINE,
    )
    if not match:
        raise ParityError(f"Native constexpr not found: {name}")
    return strip_inline_comment(match.group(1)).strip()


def native_constant(text: str, name: str) -> str:
    """Read a native migration constant whether it is a #define or constexpr."""
    define_match = re.search(
        rf"^\s*#define\s+{re.escape(name)}\s+(.+?)\s*$",
        text,
        re.MULTILINE,
    )
    if define_match:
        return strip_inline_comment(define_match.group(1)).strip()

    match = re.search(
        rf"constexpr\s+[^;=]+?\b{re.escape(name)}\s*=\s*([^;]+);",
        text,
        re.MULTILINE,
    )
    if match:
        return strip_inline_comment(match.group(1)).strip()

    raise ParityError(f"Native constant not found (#define or constexpr): {name}")


def canonical_scalar(value: str) -> str:
    value = value.strip()

    # Remove wrapping parentheses used by a few Arduino macros.
    while value.startswith("(") and value.endswith(")"):
        value = value[1:-1].strip()

    if value.startswith('"') and value.endswith('"'):
        return value[1:-1]

    gpio = re.fullmatch(r"GPIO_NUM_(\d+)", value)
    if gpio:
        return gpio.group(1)

    # Numeric constants used in this project are simple decimal / hex / float
    # literals. Strip common C/C++ suffixes before numeric comparison.
    numeric = re.fullmatch(
        r"([+-]?(?:0[xX][0-9A-Fa-f]+|\d+(?:\.\d*)?|\.\d+))(?:[fFuUlL]+)?",
        value,
    )
    if numeric:
        literal = numeric.group(1)
        if re.fullmatch(r"[+-]?0[xX][0-9A-Fa-f]+", literal):
            return str(int(literal, 0))
        if "." in literal:
            return f"{float(literal):.9g}"
        return str(int(literal, 10))

    return value


def native_uuid(text: str, variable: str) -> str:
    match = re.search(
        rf"static\s+const\s+ble_uuid128_t\s+{re.escape(variable)}\s*=\s*BLE_UUID128_INIT\((.*?)\);",
        text,
        re.DOTALL,
    )
    if not match:
        raise ParityError(f"Native UUID definition not found: {variable}")

    byte_values = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(1))]
    if len(byte_values) != 16:
        raise ParityError(f"Native UUID {variable} has {len(byte_values)} bytes, expected 16")

    canonical = bytes(reversed(byte_values)).hex().upper()
    return (
        f"{canonical[0:8]}-{canonical[8:12]}-{canonical[12:16]}-"
        f"{canonical[16:20]}-{canonical[20:32]}"
    )


def require_token(text: str, token: str, description: str) -> None:
    if token not in text:
        raise ParityError(f"{description}: expected token not found: {token}")
    print(f"[PASS] {description}: {token}")


def compare_scalar(
    arduino_text: str,
    native_text: str,
    arduino_name: str,
    native_name: str,
) -> None:
    left_raw = macro(arduino_text, arduino_name, "Arduino macro")
    right_raw = native_constant(native_text, native_name)
    left = canonical_scalar(left_raw)
    right = canonical_scalar(right_raw)
    if left != right:
        raise ParityError(
            f"{arduino_name} != {native_name}: "
            f"Arduino={left_raw!r} ({left}), Native={right_raw!r} ({right})"
        )
    print(f"[PASS] {arduino_name} == {native_name}: {left}")


def compare_uuid(
    arduino_text: str,
    ble_text: str,
    arduino_name: str,
    native_variable: str,
) -> None:
    left = canonical_scalar(macro(arduino_text, arduino_name, "Arduino UUID macro")).upper()
    right = native_uuid(ble_text, native_variable).upper()
    if left != right:
        raise ParityError(
            f"UUID mismatch {arduino_name}/{native_variable}: Arduino={left}, Native={right}"
        )
    print(f"[PASS] UUID {arduino_name}: {left}")


def main() -> int:
    arduino = read(ARDUINO_CONFIG)
    native = read(NATIVE_CONFIG)
    ble = read(NATIVE_BLE)
    camera = read(NATIVE_CAMERA)

    scalar_pairs = [
        # Device metadata
        ("BLE_DEVICE_NAME", "OMI_BLE_DEVICE_NAME"),
        ("FIRMWARE_VERSION_STRING", "OMI_FIRMWARE_VERSION_STRING"),
        ("HARDWARE_REVISION", "OMI_HARDWARE_REVISION"),
        ("MANUFACTURER_NAME", "OMI_MANUFACTURER_NAME"),
        # Board
        ("POWER_BUTTON_PIN", "OMI_POWER_BUTTON_GPIO"),
        ("STATUS_LED_PIN", "OMI_STATUS_LED_GPIO"),
        ("BUTTON_DEBOUNCE_MS", "OMI_BUTTON_DEBOUNCE_MS"),
        ("POWER_OFF_PRESS_MS", "OMI_POWER_OFF_PRESS_MS"),
        # Microphone
        ("MIC_CLK_PIN", "OMI_MIC_CLK_GPIO"),
        ("MIC_DATA_PIN", "OMI_MIC_DATA_GPIO"),
        ("MIC_SAMPLE_RATE", "OMI_MIC_SAMPLE_RATE"),
        ("MIC_BUFFER_SAMPLES", "OMI_MIC_BUFFER_SAMPLES"),
        ("MIC_GAIN", "OMI_MIC_GAIN"),
        ("AUDIO_RING_BUFFER_SAMPLES", "OMI_AUDIO_RING_BUFFER_SAMPLES"),
        # Opus / audio wire protocol
        ("AUDIO_CODEC_ID", "OMI_AUDIO_CODEC_ID"),
        ("OPUS_FRAME_SAMPLES", "OMI_OPUS_FRAME_SAMPLES"),
        ("OPUS_OUTPUT_MAX_BYTES", "OMI_OPUS_OUTPUT_MAX_BYTES"),
        ("OPUS_BITRATE", "OMI_OPUS_BITRATE"),
        ("OPUS_COMPLEXITY", "OMI_OPUS_COMPLEXITY"),
        ("OPUS_VBR", "OMI_OPUS_VBR"),
        ("AUDIO_PACKET_HEADER_SIZE", "OMI_AUDIO_PACKET_HEADER_SIZE"),
        # BLE
        ("BLE_MTU_SIZE", "OMI_BLE_MTU_SIZE"),
        ("BLE_CONN_MIN_INTERVAL", "OMI_BLE_CONN_MIN_INTERVAL"),
        ("BLE_CONN_MAX_INTERVAL", "OMI_BLE_CONN_MAX_INTERVAL"),
        ("BLE_CONN_LATENCY", "OMI_BLE_CONN_LATENCY"),
        ("BLE_CONN_TIMEOUT", "OMI_BLE_CONN_TIMEOUT"),
        # Camera
        ("XCLK_GPIO_NUM", "OMI_CAMERA_XCLK_GPIO"),
        ("SIOD_GPIO_NUM", "OMI_CAMERA_SIOD_GPIO"),
        ("SIOC_GPIO_NUM", "OMI_CAMERA_SIOC_GPIO"),
        ("Y9_GPIO_NUM", "OMI_CAMERA_Y9_GPIO"),
        ("Y8_GPIO_NUM", "OMI_CAMERA_Y8_GPIO"),
        ("Y7_GPIO_NUM", "OMI_CAMERA_Y7_GPIO"),
        ("Y6_GPIO_NUM", "OMI_CAMERA_Y6_GPIO"),
        ("Y5_GPIO_NUM", "OMI_CAMERA_Y5_GPIO"),
        ("Y4_GPIO_NUM", "OMI_CAMERA_Y4_GPIO"),
        ("Y3_GPIO_NUM", "OMI_CAMERA_Y3_GPIO"),
        ("Y2_GPIO_NUM", "OMI_CAMERA_Y2_GPIO"),
        ("VSYNC_GPIO_NUM", "OMI_CAMERA_VSYNC_GPIO"),
        ("HREF_GPIO_NUM", "OMI_CAMERA_HREF_GPIO"),
        ("PCLK_GPIO_NUM", "OMI_CAMERA_PCLK_GPIO"),
        ("CAMERA_XCLK_FREQ", "OMI_CAMERA_XCLK_HZ"),
        ("CAMERA_JPEG_QUALITY", "OMI_CAMERA_JPEG_QUALITY"),
        ("PHOTO_CAPTURE_INTERVAL_MS", "OMI_CAMERA_CAPTURE_INTERVAL_MS"),
        # Battery
        ("BATTERY_ADC_PIN", "OMI_BATTERY_ADC_GPIO"),
        ("VOLTAGE_DIVIDER_RATIO", "OMI_BATTERY_VOLTAGE_DIVIDER_RATIO"),
        ("BATTERY_MAX_VOLTAGE", "OMI_BATTERY_MAX_VOLTAGE"),
        ("BATTERY_MIN_VOLTAGE", "OMI_BATTERY_MIN_VOLTAGE"),
        ("BATTERY_CRITICAL_VOLTAGE", "OMI_BATTERY_CRITICAL_VOLTAGE"),
        ("BATTERY_LOW_VOLTAGE", "OMI_BATTERY_LOW_VOLTAGE"),
        ("BATTERY_TASK_INTERVAL_MS", "OMI_BATTERY_CHECK_INTERVAL_MS"),
        # OTA commands
        ("OTA_CMD_SET_WIFI", "OMI_OTA_CMD_SET_WIFI"),
        ("OTA_CMD_START_OTA", "OMI_OTA_CMD_START_OTA"),
        ("OTA_CMD_CANCEL_OTA", "OMI_OTA_CMD_CANCEL_OTA"),
        ("OTA_CMD_GET_STATUS", "OMI_OTA_CMD_GET_STATUS"),
        ("OTA_CMD_SET_URL", "OMI_OTA_CMD_SET_URL"),
        # OTA status
        ("OTA_STATUS_IDLE", "OMI_OTA_STATUS_IDLE"),
        ("OTA_STATUS_WIFI_CONNECTING", "OMI_OTA_STATUS_WIFI_CONNECTING"),
        ("OTA_STATUS_WIFI_CONNECTED", "OMI_OTA_STATUS_WIFI_CONNECTED"),
        ("OTA_STATUS_WIFI_FAILED", "OMI_OTA_STATUS_WIFI_FAILED"),
        ("OTA_STATUS_DOWNLOADING", "OMI_OTA_STATUS_DOWNLOADING"),
        ("OTA_STATUS_DOWNLOAD_COMPLETE", "OMI_OTA_STATUS_DOWNLOAD_COMPLETE"),
        ("OTA_STATUS_DOWNLOAD_FAILED", "OMI_OTA_STATUS_DOWNLOAD_FAILED"),
        ("OTA_STATUS_INSTALLING", "OMI_OTA_STATUS_INSTALLING"),
        ("OTA_STATUS_INSTALL_COMPLETE", "OMI_OTA_STATUS_INSTALL_COMPLETE"),
        ("OTA_STATUS_INSTALL_FAILED", "OMI_OTA_STATUS_INSTALL_FAILED"),
        ("OTA_STATUS_REBOOTING", "OMI_OTA_STATUS_REBOOTING"),
        ("OTA_STATUS_ERROR", "OMI_OTA_STATUS_ERROR"),
        ("WIFI_CONNECT_TIMEOUT_MS", "OMI_WIFI_CONNECT_TIMEOUT_MS"),
        ("WIFI_MAX_SSID_LEN", "OMI_WIFI_MAX_SSID_LEN"),
        ("WIFI_MAX_PASS_LEN", "OMI_WIFI_MAX_PASSWORD_LEN"),
        ("OTA_MAX_URL_LEN", "OMI_OTA_MAX_URL_LEN"),
    ]

    uuid_pairs = [
        ("OMI_SERVICE_UUID", "OMI_SERVICE_UUID"),
        ("AUDIO_DATA_UUID", "AUDIO_DATA_UUID"),
        ("AUDIO_CODEC_UUID", "AUDIO_CODEC_UUID"),
        ("PHOTO_DATA_UUID", "PHOTO_DATA_UUID"),
        ("PHOTO_CONTROL_UUID", "PHOTO_CONTROL_UUID"),
        ("OTA_SERVICE_UUID", "OTA_SERVICE_UUID"),
        ("OTA_CONTROL_UUID", "OTA_CONTROL_UUID"),
        ("OTA_DATA_UUID", "OTA_DATA_UUID"),
    ]

    try:
        for arduino_name, native_name in scalar_pairs:
            compare_scalar(arduino, native, arduino_name, native_name)

        for arduino_name, native_name in uuid_pairs:
            compare_uuid(arduino, ble, arduino_name, native_name)

        # Frame-size enum is defined by esp32-camera rather than our config header.
        require_token(camera, "FRAMESIZE_VGA", "Camera frame size parity")
        require_token(camera, "PIXFORMAT_JPEG", "Camera pixel format parity")

        # Wire packet invariants that do not have direct Arduino macros for every
        # field but must not drift during later refactoring.
        require_token(native, "OMI_AUDIO_SUB_INDEX = 0", "Audio sub-index remains zero")
        require_token(native, "OMI_PHOTO_FIRST_CHUNK_JPEG_BYTES = 199", "Photo first JPEG payload")
        require_token(native, "OMI_PHOTO_OTHER_CHUNK_JPEG_BYTES = 200", "Photo subsequent JPEG payload")
        require_token(native, "OMI_PHOTO_PACKET_MAX_BYTES = 202", "Photo packet maximum")
        require_token(native, "OMI_PHOTO_END_MARKER = 0xFFFF", "Photo end marker")
        require_token(native, "OMI_CAMERA_FIXED_ORIENTATION = 2", "Photo orientation byte")

    except ParityError as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1

    print("\nPhase 11 source parity checks passed.")
    print(f"Checked {len(scalar_pairs)} scalar mappings, {len(uuid_pairs)} UUIDs, and packet invariants.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
