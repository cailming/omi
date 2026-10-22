#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Camera framebuffer 的只读视图。
// data 指针只在下一次 camera_service_release_frame() 之前有效。
struct CameraFrameView {
    const uint8_t *data = nullptr;
    size_t len = 0;
    size_t width = 0;
    size_t height = 0;
    uint8_t orientation = 0;
};

// 初始化 XIAO ESP32S3 Sense Camera。
esp_err_t camera_service_init();

// 释放 Camera 驱动及当前 framebuffer。
void camera_service_deinit();

// 查询 Camera 是否已经成功初始化。
bool camera_service_is_ready();

// 获取一帧 JPEG。
// 成功后 framebuffer 会保留在 Camera 服务中，调用方使用完必须 release。
esp_err_t camera_service_capture(CameraFrameView *view);

// 归还当前 framebuffer 给 esp32-camera 驱动。
void camera_service_release_frame();

// 查询当前是否持有尚未归还的 framebuffer。
bool camera_service_has_frame();
