#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// OTA 状态变化回调。
// status / progress 沿用原 Omi BLE OTA 协议中的 2-byte 状态格式。
using OtaStatusHandler = void (*)(uint8_t status, uint8_t progress);

// 初始化 Phase 10 OTA 状态、网络事件基础设施；此时不会主动打开 Wi-Fi。
esp_err_t ota_service_init();

// 在进入 Deep Sleep 等场景下释放 OTA / Wi-Fi 资源。
// 调用前应确保 ota_service_is_busy()==false。
void ota_service_deinit();

// 设置 OTA 状态回调。通常由 app_main 转发到 BLE OTA Data Characteristic。
void ota_service_set_status_handler(OtaStatusHandler handler);

// 处理来自 OTA Control Characteristic 的原始命令。
// 该函数只做快速解析/状态更新；真正的 Wi-Fi + HTTPS OTA 在独立 FreeRTOS Task 中执行。
void ota_service_handle_command(const uint8_t *data, size_t length);

// 请求取消正在执行的 OTA；真正停止发生在 OTA Task 下一次可安全检查的位置。
void ota_service_cancel();

// 查询 OTA Task 是否正在运行。
bool ota_service_is_busy();

// 查询最近状态和进度，便于日志或其他模块读取。
uint8_t ota_service_status();
uint8_t ota_service_progress();
