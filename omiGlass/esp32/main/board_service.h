#pragma once

#include <cstdint>

#include "esp_err.h"

// 板级按键事件。后续电源管理模块会订阅 LONG_PRESS 事件完成真正的关机/深睡。
enum class BoardButtonEvent {
    NONE = 0,
    SHORT_PRESS,
    LONG_PRESS,
};

// 初始化 GPIO、按键中断、LED 和设备序列号。
esp_err_t board_service_init();

// 在主循环中周期调用，负责按键去抖和 LED 状态更新。
void board_service_process();

// 读取并清除最近一次按键事件。
BoardButtonEvent board_service_take_button_event();

// 为后续 BLE 模块预留：连接状态变化后更新 LED 表现。
void board_service_set_connected(bool connected);

// 替代 Arduino millis()，返回启动后的毫秒数。
uint64_t board_millis();

// 替代 Arduino delay()，内部使用 FreeRTOS 延时。
void board_delay_ms(uint32_t delay_ms);

// 返回基于 eFuse 默认 MAC 生成的稳定设备序列号。
// 格式为 12 位大写十六进制字符串，例如 AABBCCDDEEFF。
const char *board_device_serial();
