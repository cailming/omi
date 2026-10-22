#pragma once

#include <cstdint>

#include "esp_err.h"

// 一次电池检测结果。
struct BatteryReading {
    int raw_average = 0;                    // 10 次 ADC 原始值平均值
    int adc_millivolts = 0;                // 当前实际采用的分压点毫伏值
    float battery_voltage = 0.0f;          // 当前实际采用、乘回分压比后的电池电压
    int raw_approx_millivolts = 0;         // Arduino-style raw/4095*3300 近似毫伏值
    float raw_approx_battery_voltage = 0;  // Arduino-style 近似路径乘回分压比后的电池电压
    int calibrated_millivolts = 0;         // Curve Fitting 成功时的分压点毫伏值
    float calibrated_battery_voltage = 0;  // Curve Fitting 路径乘回分压比后的电池电压
    uint8_t percentage = 0xFF;             // 0~100 有效；0xFF 表示尚未完成有效检测
    bool calibrated = false;               // 本次最终电压是否采用 ESP-IDF ADC 校准结果
    bool valid = false;                    // 是否已经完成过一次成功的完整检测
};

// 初始化 GPIO2 对应的 ESP-IDF v6.1 ADC oneshot 通道和 Curve Fitting 校准。
esp_err_t battery_service_init();

// 释放 ADC oneshot 与校准资源。
void battery_service_deinit();

// 查询 Battery ADC 是否已经初始化。
bool battery_service_is_ready();

// 执行一次完整检测：10 次采样 -> 平均 -> 校准/近似双路径 -> 分压换算 -> 百分比平滑。
esp_err_t battery_service_read(BatteryReading *reading);

// 返回最近一次成功检测结果；尚未成功检测时 valid=false、percentage=0xFF。
BatteryReading battery_service_latest();
