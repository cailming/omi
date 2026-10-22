#!/usr/bin/env python3
"""Phase 11 串口运行日志探针。

使用方式：
    idf.py monitor | tee omiglass.log
    python3 tools/phase11_serial_probe.py omiglass.log

默认会检查启动链路、PDM/Opus 的基本速率、Battery 双路径诊断以及崩溃关键字。
它不能证明声音质量、功耗、电压精度等物理指标，但可以把大量人工看日志的工作自动化。
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
from pathlib import Path


STARTUP_TOKENS = [
    "OmiGlass 原生 ESP-IDF 固件启动",
    "Phase 9 Power Management 已启动",
    "Phase 10 Wi-Fi / HTTPS OTA 已启动",
    "Phase 4 BLE Core 已启动",
    "Phase 8 Battery ADC 已启动",
    "Phase 5 BLE Audio TX 已启动",
    "Phase 6 Camera 已启动",
    "Phase 7 Photo BLE 已启动",
    "Phase 3 Opus 编码器已启动",
    "Phase 2 PDM 麦克风已启动",
]

FATAL_TOKENS = [
    "Guru Meditation Error",
    "assert failed",
    "abort() was called",
    "panic'ed",
    "Backtrace:",
]

MIC_RE = re.compile(r"麦克风：最近约 1 秒收到 (\d+) samples，峰值=(\d+)")
OPUS_RE = re.compile(r"Opus：最近约 1 秒编码 (\d+) 帧，共 (\d+) 字节")
BATTERY_RE = re.compile(
    r"Battery：raw=(\d+) rawApprox=(\d+)mV/([0-9.]+)V "
    r"(?:calibrated=(\d+)mV/([0-9.]+)V|calibrated=不可用) "
    r"selected=([0-9.]+)V percentage=(\d+)%"
)
POWER_RE = re.compile(r"Power：state=([A-Z_]+) battery=([0-9.]+)V/(\d+)%")
AUDIO_TX_RE = re.compile(
    r"BLE Audio：.*?发送=(\d+).*?队列满丢帧=(\d+).*?MTU丢帧=(\d+).*?notify错误=(\d+).*?当前队列=(\d+)"
)


class ProbeFailure(RuntimeError):
    pass


def load_text(path: str | None) -> str:
    if path:
        return Path(path).read_text(encoding="utf-8", errors="replace")
    return sys.stdin.read()


def check(condition: bool, message: str) -> None:
    if not condition:
        raise ProbeFailure(message)
    print(f"[PASS] {message}")


def mean(values: list[int]) -> float:
    return statistics.mean(values) if values else 0.0


def main() -> int:
    parser = argparse.ArgumentParser(description="OmiGlass Phase 11 serial runtime probe")
    parser.add_argument("log", nargs="?", help="串口日志文件；省略时从 stdin 读取")
    parser.add_argument("--boot-only", action="store_true", help="只检查启动链路和崩溃关键字")
    parser.add_argument("--allow-no-calibration", action="store_true", help="允许 ADC Curve Fitting 不可用")
    args = parser.parse_args()

    text = load_text(args.log)
    if not text.strip():
        print("[FAIL] empty serial log", file=sys.stderr)
        return 1

    try:
        for token in FATAL_TOKENS:
            check(token not in text, f"未出现致命错误关键字: {token}")

        for token in STARTUP_TOKENS:
            check(token in text, f"启动阶段存在: {token}")

        if args.boot_only:
            print("\nPhase 11 serial boot probe PASSED.")
            return 0

        mic = [(int(samples), int(peak)) for samples, peak in MIC_RE.findall(text)]
        opus = [(int(frames), int(total_bytes)) for frames, total_bytes in OPUS_RE.findall(text)]
        batteries = BATTERY_RE.findall(text)
        powers = POWER_RE.findall(text)

        check(bool(mic), "检测到 Mic 运行统计")
        check(bool(opus), "检测到 Opus 运行统计")
        check(bool(batteries), "检测到 Battery 双路径诊断日志")
        check(bool(powers), "检测到 Power 状态日志")

        # app_main 约每秒打印一次统计，因此这里只做宽容的健康区间检查。
        # 真正的采样率/时钟准确度仍需真机音频分析裁决。
        mic_samples = [samples for samples, _ in mic]
        opus_frames = [frames for frames, _ in opus]
        avg_mic = mean(mic_samples)
        avg_opus = mean(opus_frames)

        check(12000 <= avg_mic <= 20000, f"Mic 平均采样量约 16k/s：{avg_mic:.1f}")
        check(35 <= avg_opus <= 65, f"Opus 平均帧率约 50fps：{avg_opus:.1f}")
        check(any(peak > 0 for _, peak in mic), "Mic PCM 峰值不是全 0")

        battery_percentages = [int(row[-1]) for row in batteries]
        check(all(0 <= value <= 100 for value in battery_percentages), "Battery 百分比均在 0~100")

        calibrated_rows = [row for row in batteries if row[3] and row[4]]
        if calibrated_rows:
            deltas = [abs(float(row[4]) - float(row[2])) for row in calibrated_rows]
            print(
                f"[INFO] Battery calibrated-vs-raw：样本={len(deltas)} "
                f"平均差={statistics.mean(deltas):.3f}V 最大差={max(deltas):.3f}V"
            )
        elif args.allow_no_calibration:
            print("[WARN] 日志中没有成功的 ADC calibrated 路径；已按参数允许继续")
        else:
            raise ProbeFailure("未检测到成功的 ADC Curve Fitting 结果；如硬件确实不支持可加 --allow-no-calibration")

        audio_rows = AUDIO_TX_RE.findall(text)
        if audio_rows:
            notify_errors = sum(int(row[3]) for row in audio_rows)
            print(f"[INFO] BLE Audio 统计行={len(audio_rows)} notify errors={notify_errors}")

        states = sorted({row[0] for row in powers})
        print(f"[INFO] 观察到 Power states: {', '.join(states)}")

    except ProbeFailure as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1

    print("\nPhase 11 serial runtime probe PASSED.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
