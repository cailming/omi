#pragma once

#include <cstdint>

#include "esp_err.h"

// Phase 9 统一电源状态。
enum class PowerState {
    ACTIVE = 0,
    POWER_SAVE,
    LOW_BATTERY,
    POWERING_OFF,
};

// 进入 Deep Sleep 的原因，便于日志和后续诊断。
enum class PowerShutdownReason {
    NONE = 0,
    USER_LONG_PRESS,
    CRITICAL_BATTERY,
    IDLE_TIMEOUT,
};

// 主循环每轮提供给电源管理模块的运行状态。
struct PowerServiceInputs {
    bool ble_connected = false;
    bool audio_streaming = false;
    bool photo_uploading = false;

    bool battery_valid = false;
    float battery_voltage = 0.0f;

    bool short_press = false;
    bool long_press = false;
};

// Deep Sleep 前由上层释放业务资源。
using PowerPrepareDeepSleepHandler = void (*)();

// 初始化 ESP-IDF v6.1 Power Management：
// 80MHz / 40MHz DFS、自动 Light Sleep、CPU/Light-Sleep PM Lock。
esp_err_t power_service_init();

// 释放 PM Lock 资源。正常运行通常不会主动调用；主要用于测试或重初始化。
void power_service_deinit();

// 注册 Deep Sleep 前的资源释放回调。
void power_service_set_prepare_deep_sleep_handler(PowerPrepareDeepSleepHandler handler);

// 每轮主循环调用一次，执行状态切换、PM Lock 调度、LED 提示和 Deep Sleep 判定。
void power_service_process(uint64_t now_ms, const PowerServiceInputs &inputs);

// 当前电源状态。
PowerState power_service_state();

// 当前正在等待执行的关机原因；正常状态返回 NONE。
PowerShutdownReason power_service_shutdown_reason();

// 返回便于日志显示的状态名称。
const char *power_service_state_name();
