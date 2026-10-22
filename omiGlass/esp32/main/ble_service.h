#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// 初始化 NimBLE、注册 Omi GATT 服务并开始广播。
// device_serial 由板级服务生成后传入，BLE 层不关心其生成方式。
esp_err_t ble_service_init(const char *device_serial);

// 查询 BLE 核心是否已经完成初始化。
bool ble_service_is_ready();

// 查询当前是否存在 BLE 连接。
bool ble_service_is_connected();

// 查询客户端是否订阅了 Audio Data 通知。
bool ble_service_audio_subscribed();

// 查询客户端是否订阅了 Photo Data 通知。
bool ble_service_photo_subscribed();

// 查询客户端是否订阅了 Battery Level 通知。
bool ble_service_battery_subscribed();

// 查询客户端是否订阅了 OTA Data 通知。
bool ble_service_ota_subscribed();

// 返回当前协商出的 ATT MTU；未连接时返回默认 ATT MTU 23。
uint16_t ble_service_mtu();

// Phase 5：通过 Audio Data Characteristic 发送一包已经封装好的实时音频数据。
// packet 必须已经包含 Omi 的 3 字节音频包头。
// 函数内部使用 ble_gatts_notify_custom()；NimBLE 会接管并释放 mbuf。
esp_err_t ble_service_notify_audio(const uint8_t *packet, size_t len);
