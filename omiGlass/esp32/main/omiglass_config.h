#pragma once

#include <cstddef>
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
// Opus 编码配置
// 与原 Arduino 固件保持协议与编码参数一致。
// =============================================================================
constexpr uint8_t OMI_AUDIO_CODEC_ID = 21;
constexpr size_t OMI_OPUS_FRAME_SAMPLES = 320;          // 16kHz 下 20ms
constexpr size_t OMI_OPUS_OUTPUT_MAX_BYTES = 160;
constexpr int OMI_OPUS_BITRATE = 32000;
constexpr int OMI_OPUS_COMPLEXITY = 3;
constexpr int OMI_OPUS_VBR = 1;
constexpr size_t OMI_AUDIO_RING_BUFFER_SAMPLES = 8000;  // 500ms PCM 缓冲

// 编码帧与麦克风读取帧当前应保持一致；即使未来调整读取粒度，Opus 服务仍支持任意长度 PCM 输入。
static_assert(OMI_OPUS_FRAME_SAMPLES == 320, "Omi 当前协议要求 20ms / 320 samples Opus 帧");

// =============================================================================
// BLE 配置
// 与原 Arduino 固件保持相同的 MTU、广播间隔和连接参数。
// =============================================================================
constexpr uint16_t OMI_BLE_MTU_SIZE = 517;

// 广播间隔单位为 0.625ms：0x0140=200ms，0x0280=400ms。
constexpr uint16_t OMI_BLE_ADV_MIN_INTERVAL = 0x0140;
constexpr uint16_t OMI_BLE_ADV_MAX_INTERVAL = 0x0280;

// 连接间隔单位为 1.25ms：20=25ms，40=50ms。
constexpr uint16_t OMI_BLE_CONN_MIN_INTERVAL = 20;
constexpr uint16_t OMI_BLE_CONN_MAX_INTERVAL = 40;
constexpr uint16_t OMI_BLE_CONN_LATENCY = 0;

// Supervision timeout 单位为 10ms：800=8 秒。
constexpr uint16_t OMI_BLE_CONN_TIMEOUT = 800;

// =============================================================================
// BLE 实时音频发送配置
// 与原 Arduino 固件保持 2 字节 packet index + 1 字节 sub-index 的包头格式。
// =============================================================================
constexpr size_t OMI_AUDIO_PACKET_HEADER_SIZE = 3;
constexpr size_t OMI_AUDIO_TX_QUEUE_FRAMES = 16;
constexpr size_t OMI_AUDIO_TX_MAX_PACKETS_PER_PROCESS = 2;
constexpr uint8_t OMI_AUDIO_SUB_INDEX = 0;

// Audio Data 一次通知的最大业务负载：3 字节包头 + 一帧 Opus。
constexpr size_t OMI_AUDIO_TX_PACKET_MAX_BYTES =
    OMI_AUDIO_PACKET_HEADER_SIZE + OMI_OPUS_OUTPUT_MAX_BYTES;

static_assert(
    OMI_AUDIO_TX_PACKET_MAX_BYTES <= OMI_BLE_MTU_SIZE - 3,
    "Configured BLE MTU must fit one complete Omi audio notification");

// =============================================================================
// Camera 配置
// 与原 Arduino 固件保持相同的 XIAO ESP32S3 Sense 引脚和 JPEG 参数。
// =============================================================================
constexpr int OMI_CAMERA_PWDN_GPIO = -1;
constexpr int OMI_CAMERA_RESET_GPIO = -1;
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

constexpr uint32_t OMI_CAMERA_XCLK_HZ = 6000000;       // 原固件：6MHz
constexpr int OMI_CAMERA_JPEG_QUALITY = 25;
constexpr size_t OMI_CAMERA_FRAME_BUFFER_COUNT = 1;
constexpr uint32_t OMI_CAMERA_CAPTURE_INTERVAL_MS = 30000;
constexpr uint8_t OMI_CAMERA_FIXED_ORIENTATION = 2;    // 0/90/180/270 中的 180°

// =============================================================================
// Photo BLE 发送配置
// 严格保持原 Arduino 固件的图片分包格式。
// =============================================================================
constexpr size_t OMI_PHOTO_FRAME_INDEX_BYTES = 2;
constexpr size_t OMI_PHOTO_FIRST_CHUNK_HEADER_BYTES = 3; // frame index + orientation
constexpr size_t OMI_PHOTO_OTHER_CHUNK_HEADER_BYTES = 2; // frame index
constexpr size_t OMI_PHOTO_FIRST_CHUNK_JPEG_BYTES = 199;
constexpr size_t OMI_PHOTO_OTHER_CHUNK_JPEG_BYTES = 200;
constexpr size_t OMI_PHOTO_PACKET_MAX_BYTES = 202;
constexpr size_t OMI_PHOTO_MAX_CHUNKS_PER_PROCESS = 2;
constexpr uint16_t OMI_PHOTO_END_MARKER = 0xFFFF;

static_assert(
    OMI_PHOTO_FIRST_CHUNK_HEADER_BYTES + OMI_PHOTO_FIRST_CHUNK_JPEG_BYTES == OMI_PHOTO_PACKET_MAX_BYTES,
    "Omi 首个图片分片必须保持 202 字节上限");
static_assert(
    OMI_PHOTO_OTHER_CHUNK_HEADER_BYTES + OMI_PHOTO_OTHER_CHUNK_JPEG_BYTES == OMI_PHOTO_PACKET_MAX_BYTES,
    "Omi 后续图片分片必须保持 202 字节上限");
static_assert(
    OMI_PHOTO_PACKET_MAX_BYTES <= OMI_BLE_MTU_SIZE - 3,
    "Configured BLE MTU must fit one complete Omi photo notification");

// =============================================================================
// Battery ADC 配置
// 保持原 Arduino 固件的 GPIO、分压参数、采样次数和检测周期。
// =============================================================================
constexpr gpio_num_t OMI_BATTERY_ADC_GPIO = GPIO_NUM_2;
constexpr float OMI_BATTERY_VOLTAGE_DIVIDER_RATIO = 6.086f;
constexpr float OMI_BATTERY_MAX_VOLTAGE = 4.2f;
constexpr float OMI_BATTERY_MIN_VOLTAGE = 3.2f;
constexpr float OMI_BATTERY_CRITICAL_VOLTAGE = 3.3f;
constexpr float OMI_BATTERY_LOW_VOLTAGE = 3.4f;

// 原固件每 20 秒做一次内部检测；单次检测连续读取 10 次，每次间隔 10ms。
constexpr uint32_t OMI_BATTERY_CHECK_INTERVAL_MS = 20000;
constexpr uint32_t OMI_BATTERY_REPORT_INTERVAL_MS = 90000;
constexpr size_t OMI_BATTERY_SAMPLE_COUNT = 10;
constexpr uint32_t OMI_BATTERY_SAMPLE_DELAY_MS = 10;

// 对最终电池电压做合理范围保护，保持原固件行为。
constexpr float OMI_BATTERY_VOLTAGE_CLAMP_MIN = 2.5f;
constexpr float OMI_BATTERY_VOLTAGE_CLAMP_MAX = 5.0f;
