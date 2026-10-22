#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Phase 7 图片采集 / BLE 上传统计。
struct PhotoServiceStats {
    uint32_t captures_started = 0;
    uint32_t captures_completed = 0;
    uint32_t capture_errors = 0;
    uint32_t uploads_cancelled = 0;
    uint32_t chunks_sent = 0;
    uint32_t jpeg_bytes_sent = 0;
    uint32_t yielded_to_audio = 0;
    uint32_t mtu_waits = 0;
    uint32_t notify_errors = 0;
    bool uploading = false;
    size_t remaining_bytes = 0;
};

// 初始化 Photo 业务状态。
// 默认行为与原 Arduino 固件一致：连接并订阅 Photo Data 后立即拍一张，之后每 30 秒拍一张。
esp_err_t photo_service_init();

// 在主业务循环中周期调用。
// 内部处理 Photo Control、拍照、BLE 分包发送和 framebuffer 释放。
void photo_service_process(uint64_t now_ms);

// 查询当前是否正在上传图片。
bool photo_service_is_uploading();

// 取消当前图片上传并归还 framebuffer。
void photo_service_cancel_upload();

// 读取并清空最近一个统计周期的计数。
PhotoServiceStats photo_service_take_stats();
