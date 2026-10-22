#!/usr/bin/env python3
"""Phase 11 运行时契约静态检查。

这个脚本不假装替代真机测试；它负责把仍然能从源码证明的行为锁进 CI：
- Button / LED / Power 时间阈值；
- Audio 高于 Photo 的调度顺序；
- OTA 维护模式和关机安全顺序；
- Deep Sleep 的 GPIO1 唤醒与按键释放保护；
- ESP-IDF PM / Tickless / BLE Modem Sleep 配置。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARDUINO_CONFIG = (ROOT.parent / "firmware" / "src" / "config.h").read_text(encoding="utf-8")
ARDUINO_APP = (ROOT.parent / "firmware" / "src" / "app.cpp").read_text(encoding="utf-8")
CONFIG = (ROOT / "main" / "omiglass_config.h").read_text(encoding="utf-8")
BOARD = (ROOT / "main" / "board_service.cpp").read_text(encoding="utf-8")
POWER = (ROOT / "main" / "power_service.cpp").read_text(encoding="utf-8")
APP = (ROOT / "main" / "app_main.cpp").read_text(encoding="utf-8")
PHOTO = (ROOT / "main" / "photo_service.cpp").read_text(encoding="utf-8")
SDKCONFIG = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")


class ContractError(RuntimeError):
    pass


def strip_comment(value: str) -> str:
    return value.split("//", 1)[0].strip()


def macro_int(text: str, name: str) -> int:
    match = re.search(rf"^\s*#define\s+{re.escape(name)}\s+(.+?)\s*$", text, re.MULTILINE)
    if not match:
        raise ContractError(f"Arduino macro not found: {name}")
    raw = strip_comment(match.group(1))
    raw = re.sub(r"[uUlLfF]+$", "", raw)
    return int(raw, 0)


def constexpr_int(text: str, name: str) -> int:
    match = re.search(
        rf"constexpr\s+[^;=]+?\b{re.escape(name)}\s*=\s*([^;]+);",
        text,
        re.MULTILINE,
    )
    if not match:
        raise ContractError(f"Native constexpr not found: {name}")
    raw = strip_comment(match.group(1))
    raw = re.sub(r"[uUlL]+$", "", raw)
    return int(raw, 0)


def compare_int(arduino_name: str, native_name: str) -> None:
    left = macro_int(ARDUINO_CONFIG, arduino_name)
    right = constexpr_int(CONFIG, native_name)
    if left != right:
        raise ContractError(f"{arduino_name}={left} != {native_name}={right}")
    print(f"[PASS] {arduino_name} == {native_name}: {left}")


def require(text: str, token: str, description: str) -> None:
    if token not in text:
        raise ContractError(f"{description}: missing token {token!r}")
    print(f"[PASS] {description}")


def require_order(text: str, first: str, second: str, description: str) -> None:
    first_pos = text.find(first)
    second_pos = text.find(second)
    if first_pos < 0 or second_pos < 0 or first_pos >= second_pos:
        raise ContractError(f"{description}: expected {first!r} before {second!r}")
    print(f"[PASS] {description}")


def main() -> int:
    try:
        # Arduino config.h 中真正参与运行的 Power 参数。
        compare_int("NORMAL_CPU_FREQ_MHZ", "OMI_NORMAL_CPU_FREQ_MHZ")
        compare_int("MIN_CPU_FREQ_MHZ", "OMI_MIN_CPU_FREQ_MHZ")
        compare_int("IDLE_THRESHOLD_MS", "OMI_POWER_SAVE_IDLE_MS")
        compare_int("DEEP_SLEEP_THRESHOLD_MS", "OMI_POWER_DEEP_SLEEP_IDLE_MS")
        compare_int("LED_BATTERY_LOW_BLINK", "OMI_LOW_BATTERY_BLINK_HALF_PERIOD_MS")

        # Arduino 的 Boot / Power-off 灯效在 app.cpp 中是硬编码，直接锁定实际行为而不是误用 config.h 中未执行的值。
        require(ARDUINO_APP, "now - bootStartTime < 1500", "Arduino Boot LED 实际持续 1.5 秒")
        require(ARDUINO_APP, "/ 150) % 2", "Arduino Boot LED 150ms 半周期")
        require(ARDUINO_APP, "now - powerOffStartTime < 800", "Arduino 关机 LED 实际持续 800ms")
        require(ARDUINO_APP, "/ 200) % 2", "Arduino 关机 LED 200ms 半周期")
        require(ARDUINO_APP, "pressDuration >= 2000", "Arduino 长按阈值 2 秒")

        require(CONFIG, "OMI_BOOT_SEQUENCE_MS = 1500", "Native Boot LED 持续 1.5 秒")
        require(CONFIG, "OMI_BOOT_BLINK_HALF_PERIOD_MS = 150", "Native Boot LED 150ms 半周期")
        require(CONFIG, "OMI_POWER_OFF_SEQUENCE_MS = 800", "Native 关机 LED 持续 800ms")
        require(CONFIG, "OMI_POWER_OFF_BLINK_HALF_PERIOD_MS = 200", "Native 关机 LED 200ms 半周期")
        require(BOARD, "OMI_POWER_OFF_PRESS_MS", "Board 长按判断使用共享配置")
        require(BOARD, "OMI_BUTTON_DEBOUNCE_MS", "Board 去抖使用共享配置")
        require(BOARD, "OMI_BOOT_SEQUENCE_MS", "Board Boot LED 使用共享配置")
        require(BOARD, "OMI_DISCONNECTED_BLINK_PERIOD_MS", "Board 断连 LED 使用共享配置")

        # Power Service 必须使用共享阈值，避免局部硬编码重新漂移。
        for token, description in [
            ("OMI_NORMAL_CPU_FREQ_MHZ", "Power 使用 80MHz 共享配置"),
            ("OMI_MIN_CPU_FREQ_MHZ", "Power 使用 40MHz 共享配置"),
            ("OMI_POWER_LIGHT_SLEEP_IDLE_MS", "Power 使用 5 秒 Light Sleep 空闲阈值"),
            ("OMI_POWER_SAVE_IDLE_MS", "Power 使用 45 秒省电阈值"),
            ("OMI_POWER_DEEP_SLEEP_IDLE_MS", "Power 使用 5 分钟 Deep Sleep 阈值"),
            ("OMI_POWER_OFF_SEQUENCE_MS", "Power 使用共享关机灯效时长"),
            ("OMI_LOW_BATTERY_BLINK_HALF_PERIOD_MS", "Power 使用共享低电灯效时序"),
        ]:
            require(POWER, token, description)

        require(POWER, "esp_pm_configure(&pm_config)", "使用 ESP-IDF Power Management")
        require(POWER, "ESP_PM_CPU_FREQ_MAX", "使用 CPU Max PM Lock")
        require(POWER, "ESP_PM_NO_LIGHT_SLEEP", "使用 No-Light-Sleep PM Lock")
        require(POWER, "esp_sleep_enable_ext1_wakeup_io", "Deep Sleep 使用 GPIO wakeup")
        require(POWER, "ESP_EXT1_WAKEUP_ANY_LOW", "GPIO1 低电平唤醒")
        require(POWER, "gpio_get_level(OMI_POWER_BUTTON_GPIO) == OMI_BUTTON_PRESSED_LEVEL", "长按关机等待按键释放")
        require(POWER, "nimble_port_stop()", "Deep Sleep 前停止 NimBLE Host")
        require(POWER, "nimble_port_deinit()", "Deep Sleep 前反初始化 NimBLE")

        # Audio 必须先于 Photo；Photo 内部还要二次检查 Audio queue。
        require_order(APP, "audio_tx_service_process();", "photo_service_process(now_ms);", "主循环 Audio 调度优先于 Photo")
        require(PHOTO, "audio_tx_service_has_pending()", "Photo 发送前二次让路 Audio")

        # OTA 执行时必须先进入维护模式，再跑数据面；关机前先等待 OTA Task 退出。
        require_order(APP, "update_ota_maintenance_mode(ota_busy, last_ota_busy);", "audio_tx_service_process();", "OTA 维护模式先于 Audio 数据面")
        require(APP, "ota_service_cancel();", "长按/低电可请求取消 OTA")
        require(APP, "s_pending_long_press_shutdown", "OTA 中长按关机延迟到 Task 退出")
        require(APP, "power_inputs.battery_valid = latest_battery.valid && !ota_busy", "OTA 期间禁止 Power Service 直接按低电睡眠")

        # Deep Sleep 资源释放顺序必须覆盖所有主要业务模块。
        for token, description in [
            ("ota_service_deinit();", "Deep Sleep 前释放 OTA/Wi-Fi"),
            ("audio_tx_service_reset_queue();", "Deep Sleep 前清空 Audio queue"),
            ("photo_service_cancel_upload();", "Deep Sleep 前取消 Photo upload"),
            ("mic_service_stop();", "Deep Sleep 前停止 Mic"),
            ("opus_service_deinit();", "Deep Sleep 前释放 Opus"),
            ("camera_service_deinit();", "Deep Sleep 前释放 Camera"),
            ("battery_service_deinit();", "Deep Sleep 前释放 Battery ADC"),
        ]:
            require(APP, token, description)

        # Kconfig 必须继续允许 Native Power 策略工作。
        for token, description in [
            ("CONFIG_PM_ENABLE=y", "PM 已启用"),
            ("CONFIG_FREERTOS_USE_TICKLESS_IDLE=y", "Tickless Idle 已启用"),
            ("CONFIG_BT_CTRL_MODEM_SLEEP=y", "BLE Modem Sleep 已启用"),
            ("CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y", "Wi-Fi/BLE 软件共存已启用"),
            ("CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=n", "OTA 明文 HTTP 已禁用"),
        ]:
            require(SDKCONFIG, token, description)

    except ContractError as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1

    print("\nPhase 11 runtime contract checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
