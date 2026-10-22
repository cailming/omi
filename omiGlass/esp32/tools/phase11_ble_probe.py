#!/usr/bin/env python3
"""Phase 11 BLE 真机探针。

默认只做安全的只读检查：扫描、连接、枚举 GATT、检查 UUID / Properties，
并读取 Device Info、Codec、Battery 和 OTA 状态。

可选能力：
- --audio-seconds N：订阅实时 Audio，检查 packet index / sub-index；
- --photo：触发一次单拍并重组 JPEG；
- --ota-status-notify：发送安全的 GET_STATUS 命令并验证 OTA Data Notify；
- --reconnect-count N：完成主测试后反复连接/断开 N 次，验证广播恢复和重连稳定性。

依赖 Bleak 3.x：
    python3 -m pip install 'bleak>=3.0,<4'
"""

from __future__ import annotations

import argparse
import asyncio
import sys
from dataclasses import dataclass, field
from typing import Iterable

try:
    from bleak import BleakClient, BleakScanner
    from bleak.backends.characteristic import BleakGATTCharacteristic
except ImportError as exc:  # pragma: no cover - host setup path
    raise SystemExit("Bleak is required: python3 -m pip install 'bleak>=3.0,<4'") from exc

DEVICE_NAME = "OMI Glass"

OMI_SERVICE_UUID = "19b10000-e8f2-537e-4f6c-d104768a1214"
AUDIO_DATA_UUID = "19b10001-e8f2-537e-4f6c-d104768a1214"
AUDIO_CODEC_UUID = "19b10002-e8f2-537e-4f6c-d104768a1214"
PHOTO_DATA_UUID = "19b10005-e8f2-537e-4f6c-d104768a1214"
PHOTO_CONTROL_UUID = "19b10006-e8f2-537e-4f6c-d104768a1214"
OTA_SERVICE_UUID = "19b10010-e8f2-537e-4f6c-d104768a1214"
OTA_CONTROL_UUID = "19b10011-e8f2-537e-4f6c-d104768a1214"
OTA_DATA_UUID = "19b10012-e8f2-537e-4f6c-d104768a1214"

BATTERY_SERVICE_UUID = "0000180f-0000-1000-8000-00805f9b34fb"
BATTERY_LEVEL_UUID = "00002a19-0000-1000-8000-00805f9b34fb"
DEVICE_INFO_SERVICE_UUID = "0000180a-0000-1000-8000-00805f9b34fb"
MANUFACTURER_UUID = "00002a29-0000-1000-8000-00805f9b34fb"
MODEL_UUID = "00002a24-0000-1000-8000-00805f9b34fb"
FIRMWARE_UUID = "00002a26-0000-1000-8000-00805f9b34fb"
HARDWARE_UUID = "00002a27-0000-1000-8000-00805f9b34fb"
SERIAL_UUID = "00002a25-0000-1000-8000-00805f9b34fb"

EXPECTED_CODEC_ID = 21
EXPECTED_ORIENTATION = 2
PHOTO_END_INDEX = 0xFFFF
OTA_CMD_GET_STATUS = 0x04

EXPECTED_SERVICES = {
    OMI_SERVICE_UUID,
    BATTERY_SERVICE_UUID,
    DEVICE_INFO_SERVICE_UUID,
    OTA_SERVICE_UUID,
}

EXPECTED_PROPERTIES = {
    AUDIO_DATA_UUID: {"read", "notify"},
    AUDIO_CODEC_UUID: {"read"},
    PHOTO_DATA_UUID: {"read", "notify"},
    PHOTO_CONTROL_UUID: {"write"},
    BATTERY_LEVEL_UUID: {"read", "notify"},
    MANUFACTURER_UUID: {"read"},
    MODEL_UUID: {"read"},
    FIRMWARE_UUID: {"read"},
    HARDWARE_UUID: {"read"},
    SERIAL_UUID: {"read"},
    OTA_CONTROL_UUID: {"read", "write"},
    OTA_DATA_UUID: {"read", "notify"},
}


class ProbeFailure(RuntimeError):
    pass


def normalize_uuid(value: str) -> str:
    return value.lower()


def check(condition: bool, message: str) -> None:
    if not condition:
        raise ProbeFailure(message)
    print(f"[PASS] {message}")


def characteristic_map(client: BleakClient) -> dict[str, BleakGATTCharacteristic]:
    result: dict[str, BleakGATTCharacteristic] = {}
    for service in client.services:
        for char in service.characteristics:
            result[normalize_uuid(char.uuid)] = char
    return result


def decode_text(value: bytearray) -> str:
    return bytes(value).decode("utf-8", errors="replace").rstrip("\x00")


@dataclass
class AudioCapture:
    packets: int = 0
    payload_bytes: int = 0
    first_index: int | None = None
    previous_index: int | None = None
    index_gaps: int = 0
    bad_sub_index: int = 0
    malformed: int = 0

    def callback(self, _char: BleakGATTCharacteristic, data: bytearray) -> None:
        raw = bytes(data)
        if len(raw) < 4:
            self.malformed += 1
            return

        packet_index = raw[0] | (raw[1] << 8)
        sub_index = raw[2]
        if sub_index != 0:
            self.bad_sub_index += 1

        if self.first_index is None:
            self.first_index = packet_index
        if self.previous_index is not None:
            expected = (self.previous_index + 1) & 0xFFFF
            if packet_index != expected:
                self.index_gaps += 1
        self.previous_index = packet_index
        self.packets += 1
        self.payload_bytes += len(raw) - 3


@dataclass
class PhotoCapture:
    done: asyncio.Event = field(default_factory=asyncio.Event)
    chunks: int = 0
    jpeg: bytearray = field(default_factory=bytearray)
    orientation: int | None = None
    expected_index: int = 0
    errors: list[str] = field(default_factory=list)

    def callback(self, _char: BleakGATTCharacteristic, data: bytearray) -> None:
        raw = bytes(data)
        if len(raw) < 2:
            self.errors.append(f"chunk too short: {len(raw)}")
            return

        index = raw[0] | (raw[1] << 8)
        if index == PHOTO_END_INDEX:
            if len(raw) != 2:
                self.errors.append(f"end marker length={len(raw)}, expected 2")
            self.done.set()
            return

        if index != self.expected_index:
            self.errors.append(f"chunk index={index}, expected={self.expected_index}")
            self.expected_index = (index + 1) & 0xFFFF
        else:
            self.expected_index = (self.expected_index + 1) & 0xFFFF

        if index == 0:
            if len(raw) < 4:
                self.errors.append(f"first chunk too short: {len(raw)}")
                return
            self.orientation = raw[2]
            payload = raw[3:]
            if len(payload) > 199:
                self.errors.append(f"first JPEG payload={len(payload)}, expected <=199")
        else:
            payload = raw[2:]
            if len(payload) > 200:
                self.errors.append(f"JPEG payload={len(payload)}, expected <=200")

        if len(raw) > 202:
            self.errors.append(f"packet length={len(raw)}, expected <=202")

        self.jpeg.extend(payload)
        self.chunks += 1


@dataclass
class OtaStatusCapture:
    event: asyncio.Event = field(default_factory=asyncio.Event)
    packets: list[bytes] = field(default_factory=list)

    def callback(self, _char: BleakGATTCharacteristic, data: bytearray) -> None:
        raw = bytes(data)
        self.packets.append(raw)
        self.event.set()


async def validate_static_gatt(client: BleakClient) -> None:
    service_uuids = {normalize_uuid(service.uuid) for service in client.services}
    for expected in EXPECTED_SERVICES:
        check(expected in service_uuids, f"Service present: {expected}")

    chars = characteristic_map(client)
    for uuid, expected_props in EXPECTED_PROPERTIES.items():
        check(uuid in chars, f"Characteristic present: {uuid}")
        actual_props = {str(p).lower() for p in chars[uuid].properties}
        check(expected_props.issubset(actual_props), f"Properties {uuid}: {sorted(actual_props)}")

    codec = bytes(await client.read_gatt_char(AUDIO_CODEC_UUID))
    check(codec == bytes([EXPECTED_CODEC_ID]), f"Audio codec ID = {EXPECTED_CODEC_ID}")

    battery = bytes(await client.read_gatt_char(BATTERY_LEVEL_UUID))
    check(len(battery) == 1 and battery[0] <= 100, f"Battery Level = {battery[0] if battery else 'missing'}%")

    expected_text = {
        MANUFACTURER_UUID: "Based Hardware",
        MODEL_UUID: DEVICE_NAME,
        FIRMWARE_UUID: "2.3.2",
        HARDWARE_UUID: "ESP32-S3-v1.0",
    }
    for uuid, expected in expected_text.items():
        actual = decode_text(await client.read_gatt_char(uuid))
        check(actual == expected, f"Device Info {uuid} = {expected!r}")

    serial = decode_text(await client.read_gatt_char(SERIAL_UUID))
    check(bool(serial) and len(serial) <= 16, f"Serial Number = {serial!r}")

    ota_control = bytes(await client.read_gatt_char(OTA_CONTROL_UUID))
    ota_data = bytes(await client.read_gatt_char(OTA_DATA_UUID))
    check(len(ota_control) == 2, f"OTA Control READ length = {len(ota_control)}")
    check(ota_control[1] == 0, f"OTA Control READ compatibility byte = {ota_control[1]}")
    check(len(ota_data) == 2, f"OTA Data READ length = {len(ota_data)}")
    check(ota_control[0] == ota_data[0], f"OTA status agrees: 0x{ota_data[0]:02X}")
    check(0 <= ota_data[1] <= 100, f"OTA progress in range: {ota_data[1]}%")

    print(f"[INFO] Client-reported ATT MTU: {client.mtu_size}")
    if client.mtu_size < 205:
        print("[WARN] MTU < 205; 202-byte Photo packets and worst-case Audio packets need a larger negotiated MTU")


async def capture_audio(client: BleakClient, seconds: float) -> None:
    capture = AudioCapture()
    await client.start_notify(AUDIO_DATA_UUID, capture.callback)
    try:
        print(f"[INFO] Capturing Audio notifications for {seconds:.1f}s...")
        await asyncio.sleep(seconds)
    finally:
        await client.stop_notify(AUDIO_DATA_UUID)

    check(capture.packets > 0, f"Audio notifications received: {capture.packets}")
    check(capture.malformed == 0, "Audio packets all have header + payload")
    check(capture.bad_sub_index == 0, "Audio sub-index remains 0")
    check(capture.index_gaps == 0, "Audio packet indexes contiguous during capture")
    avg = capture.payload_bytes / capture.packets
    print(f"[INFO] Audio Opus payload: total={capture.payload_bytes} bytes avg={avg:.1f} bytes/frame")


async def capture_photo(client: BleakClient, timeout: float) -> None:
    capture = PhotoCapture()
    await client.start_notify(PHOTO_DATA_UUID, capture.callback)
    try:
        # int8 -1 单拍命令在线上编码为 0xFF。
        await client.write_gatt_char(PHOTO_CONTROL_UUID, bytes([0xFF]), response=True)
        print(f"[INFO] Waiting up to {timeout:.1f}s for one Photo transfer...")
        await asyncio.wait_for(capture.done.wait(), timeout=timeout)
    finally:
        await client.stop_notify(PHOTO_DATA_UUID)

    check(not capture.errors, f"Photo packet sequence valid{': ' + '; '.join(capture.errors) if capture.errors else ''}")
    check(capture.chunks > 0, f"Photo chunks received: {capture.chunks}")
    check(capture.orientation == EXPECTED_ORIENTATION, f"Photo orientation = {capture.orientation} (180°)")
    check(len(capture.jpeg) >= 4, f"Photo JPEG bytes = {len(capture.jpeg)}")
    check(capture.jpeg[:2] == b"\xFF\xD8", "JPEG SOI marker FF D8")
    check(capture.jpeg[-2:] == b"\xFF\xD9", "JPEG EOI marker FF D9")


async def validate_ota_status_notify(client: BleakClient, timeout: float) -> None:
    """只发送 GET_STATUS，不修改 Wi-Fi/URL，也不会启动 OTA。"""
    capture = OtaStatusCapture()
    await client.start_notify(OTA_DATA_UUID, capture.callback)
    try:
        await client.write_gatt_char(OTA_CONTROL_UUID, bytes([OTA_CMD_GET_STATUS]), response=True)
        await asyncio.wait_for(capture.event.wait(), timeout=timeout)
    finally:
        await client.stop_notify(OTA_DATA_UUID)

    check(bool(capture.packets), "OTA GET_STATUS 收到 Notify")
    payload = capture.packets[-1]
    check(len(payload) == 2, f"OTA Notify length = {len(payload)}")
    check(0 <= payload[1] <= 100, f"OTA Notify progress in range: {payload[1]}%")
    current = bytes(await client.read_gatt_char(OTA_DATA_UUID))
    check(current == payload, f"OTA Data READ 与最近 Notify 一致: {payload.hex(' ')}")


async def stress_reconnect(device, count: int, connect_timeout: float, settle_seconds: float) -> None:
    if count <= 0:
        return

    print(f"[INFO] Starting reconnect stress: {count} cycles")
    for index in range(1, count + 1):
        if settle_seconds > 0:
            await asyncio.sleep(settle_seconds)
        async with BleakClient(device, timeout=connect_timeout) as client:
            check(client.is_connected, f"Reconnect {index}/{count}: connected")
            codec = bytes(await client.read_gatt_char(AUDIO_CODEC_UUID))
            check(codec == bytes([EXPECTED_CODEC_ID]), f"Reconnect {index}/{count}: GATT readable")
        print(f"[PASS] Reconnect {index}/{count}: disconnected cleanly")


async def run(args: argparse.Namespace) -> int:
    print(f"[INFO] Scanning for {args.name!r}...")
    device = await BleakScanner.find_device_by_name(args.name, timeout=args.scan_timeout)
    if device is None:
        raise ProbeFailure(f"Device {args.name!r} not found")

    print(f"[INFO] Found: name={device.name!r} address/id={device.address}")

    async with BleakClient(device, timeout=args.connect_timeout) as client:
        check(client.is_connected, "BLE connected")
        print(f"[INFO] Backend: {client.backend_id}")
        await validate_static_gatt(client)

        if args.ota_status_notify:
            await validate_ota_status_notify(client, args.ota_notify_timeout)

        if args.audio_seconds > 0:
            await capture_audio(client, args.audio_seconds)

        if args.photo:
            await capture_photo(client, args.photo_timeout)

    await stress_reconnect(device, args.reconnect_count, args.connect_timeout, args.reconnect_settle)

    print("\nPhase 11 BLE probe PASSED.")
    return 0


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="OmiGlass Phase 11 BLE parity probe")
    parser.add_argument("--name", default=DEVICE_NAME, help="BLE device name")
    parser.add_argument("--scan-timeout", type=float, default=15.0)
    parser.add_argument("--connect-timeout", type=float, default=30.0)
    parser.add_argument("--audio-seconds", type=float, default=0.0, help="订阅 Audio Data N 秒")
    parser.add_argument("--photo", action="store_true", help="触发并验证一次单拍")
    parser.add_argument("--photo-timeout", type=float, default=30.0)
    parser.add_argument("--ota-status-notify", action="store_true", help="发送只读 GET_STATUS 并验证 OTA Notify")
    parser.add_argument("--ota-notify-timeout", type=float, default=5.0)
    parser.add_argument("--reconnect-count", type=int, default=0, help="主测试后重复连接/断开次数，例如 20")
    parser.add_argument("--reconnect-settle", type=float, default=0.5, help="每轮重连前等待广播恢复的秒数")
    return parser.parse_args(argv)


def main() -> int:
    args = parse_args()
    try:
        return asyncio.run(run(args))
    except (ProbeFailure, asyncio.TimeoutError) as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("[FAIL] interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
