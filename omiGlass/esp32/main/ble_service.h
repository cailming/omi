#pragma once

#include <cstdint>

#include "board_service.h"
#include "esp_err.h"

// 初始化 NimBLE、注册 Omi GATT 服务并开始广播。
// Phase 4 只负责 BLE Core / GATT 结构，不发送实时音频。
esp_err_t ble_service_init();

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
