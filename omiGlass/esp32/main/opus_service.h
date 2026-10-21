#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

using OpusEncodedHandler = void (*)(const uint8_t *data, size_t len);

// 初始化 Opus 编码器及内部缓冲区。
esp_err_t opus_service_init();

// 释放 Opus 编码器和内部缓冲区。
void opus_service_deinit();

// 设置一帧 Opus 数据编码完成后的回调。
void opus_service_set_callback(OpusEncodedHandler callback);

// 接收 PCM16 单声道数据。允许一次输入任意数量的 samples。
esp_err_t opus_service_receive_pcm(const int16_t *data, size_t samples);

// 将环形缓冲区中已凑满 20ms 的 PCM 帧编码成 Opus。
void opus_service_process();

// 返回当前是否已完成初始化。
bool opus_service_is_ready();

// 返回 Omi 协议中的 Opus Codec ID。
uint8_t opus_service_codec_id();
