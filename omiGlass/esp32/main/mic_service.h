#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

using MicDataHandler = void (*)(int16_t *data, size_t samples);

// 初始化并启动 PDM 麦克风。
esp_err_t mic_service_start();

// 停止麦克风并释放 I2S 通道与缓冲区。
void mic_service_stop();

// 查询麦克风当前是否已经启动。
bool mic_service_is_running();

// 设置 PCM 数据回调。回调收到的是 16kHz、单声道、16-bit PCM。
void mic_service_set_callback(MicDataHandler callback);

// 从 I2S DMA 读取一批 PCM 数据，并在读取成功后调用回调。
void mic_service_process();
