#pragma once

#include <cstdint>

#include "driver/gpio.h"

// =============================================================================
// 设备基础信息
// =============================================================================
#define OMI_BLE_DEVICE_NAME "OMI Glass"
#define OMI_FIRMWARE_VERSION_STRING "2.3.2"
#define OMI_HARDWARE_REVISION "ESP32-S3-v1.0"
#define OMI_MANUFACTURER_NAME "Based Hardware"

// =============================================================================
// 板级 GPIO 定义
// 目标硬件：Seeed Studio XIAO ESP32S3 Sense
// =============================================================================
constexpr gpio_num_t OMI_POWER_BUTTON_GPIO = GPIO_NUM_1;
constexpr gpio_num_t OMI_STATUS_LED_GPIO = GPIO_NUM_21;

// 按键采用内部上拉，按下时为低电平。
constexpr int OMI_BUTTON_PRESSED_LEVEL = 0;

// XIAO ESP32S3 当前固件中的状态灯为反相逻辑：LOW=亮，HIGH=灭。
constexpr int OMI_LED_ON_LEVEL = 0;
constexpr int OMI_LED_OFF_LEVEL = 1;

// =============================================================================
// 按键与 LED 时序
// =============================================================================
constexpr uint32_t OMI_BUTTON_DEBOUNCE_MS = 50;
constexpr uint32_t OMI_POWER_OFF_PRESS_MS = 2000;
constexpr uint32_t OMI_BOOT_SEQUENCE_MS = 1500;
constexpr uint32_t OMI_BOOT_BLINK_HALF_PERIOD_MS = 150;
constexpr uint32_t OMI_DISCONNECTED_BLINK_PERIOD_MS = 1000;

// =============================================================================
// 麦克风配置
// XIAO ESP32S3 Sense 板载麦克风为 PDM 麦克风。
// =============================================================================
constexpr gpio_num_t OMI_MIC_CLK_GPIO = GPIO_NUM_42;
constexpr gpio_num_t OMI_MIC_DATA_GPIO = GPIO_NUM_41;
constexpr uint32_t OMI_MIC_SAMPLE_RATE = 16000;
constexpr size_t OMI_MIC_BUFFER_SAMPLES = 1600; // 总读取缓冲区容量：100ms PCM
constexpr size_t OMI_MIC_READ_SAMPLES = 320;    // 单次读取：20ms，正好对应一帧 Opus
constexpr int OMI_MIC_GAIN = 2;
constexpr uint32_t OMI_MIC_READ_TIMEOUT_MS = 30;
constexpr uint32_t OMI_MIC_DMA_DESC_NUM = 8;
constexpr uint32_t OMI_MIC_DMA_FRAME_NUM = 256;

// =============================================================================
// 后续阶段会继续使用的硬件参数
// =============================================================================
constexpr gpio_num_t OMI_BATTERY_ADC_GPIO = GPIO_NUM_2;

// Camera GPIO 映射来自原 Arduino 固件。
constexpr int OMI_CAMERA_XCLK_GPIO = 10;
constexpr int OMI_CAMERA_SIOD_GPIO = 40;
constexpr int OMI_CAMERA_SIOC_GPIO = 39;
constexpr int OMI_CAMERA_Y9_GPIO = 48;
constexpr int OMI_CAMERA_Y8_GPIO = 11;
constexpr int OMI_CAMERA_Y7_GPIO = 12;
constexpr int OMI_CAMERA_Y6_GPIO = 14;
constexpr int OMI_CAMERA_Y5_GPIO = 16;
constexpr int OMI_CAMERA_Y4_GPIO = 18;
constexpr int OMI_CAMERA_Y3_GPIO = 17;
constexpr int OMI_CAMERA_Y2_GPIO = 15;
constexpr int OMI_CAMERA_VSYNC_GPIO = 38;
constexpr int OMI_CAMERA_HREF_GPIO = 47;
constexpr int OMI_CAMERA_PCLK_GPIO = 13;
