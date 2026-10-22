#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// OTA Control 写命令回调。
// BLE 层只负责传递原始协议字节，不依赖 Wi-Fi / HTTPS OTA 的具体实现。
using BleOtaCommandHandler = void (*)(const uint8_t *data, size_t len);

// 初始化 NimBLE、注册 Omi GATT 服务并开始广播。
// device_serial 由板级服务生成后传入，BLE 层不关心其生成方式。
esp_err_t ble_service_init(const char *device_serial);

// Phase 10：注册 OTA Control 原始命令处理器。
void ble_service_set_ota_command_handler(BleOtaCommandHandler handler);

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

// Phase 5：通过 Audio Data Characteristic 发送一包通知。
esp_err_t ble_service_notify_audio(const uint8_t *data, size_t len);

// Phase 7：通过 Photo Data Characteristic 发送一包通知。
esp_err_t ble_service_notify_photo(const uint8_t *data, size_t len);

// 读取并清除最近一个 Photo Control 1-byte 命令。
// 命令按原 Arduino 固件解释为 int8_t：-1 单拍，0 停止，正值启动间隔拍照。
bool ble_service_take_photo_control(int8_t *value);

// Phase 8：更新标准 Battery Level Characteristic (0x2A19) 的最新百分比。
// notify_if_subscribed=true 时，如果客户端已经订阅 Battery Notify，会同时发送 1-byte 通知。
esp_err_t ble_service_update_battery_level(uint8_t percentage, bool notify_if_subscribed);

// Phase 10：更新 OTA Control / OTA Data 对外可读的 2-byte [status, progress] 状态。
// notify_if_subscribed=true 时，同时通过 OTA Data Characteristic 发送通知。
esp_err_t ble_service_update_ota_status(uint8_t status, uint8_t progress, bool notify_if_subscribed);
