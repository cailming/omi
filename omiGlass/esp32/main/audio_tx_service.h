#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Phase 5 的 BLE 音频发送统计。
// take_stats() 读取后会清空区间计数，但 queue_depth 保留当前队列深度。
struct AudioTxStats {
    uint32_t enqueued_frames = 0;
    uint32_t sent_frames = 0;
    uint32_t dropped_not_ready = 0;
    uint32_t dropped_queue_full = 0;
    uint32_t dropped_mtu_too_small = 0;
    uint32_t notify_errors = 0;
    size_t queue_depth = 0;
};

// 初始化发送队列和 packet index。
esp_err_t audio_tx_service_init();

// 将一帧 Opus 数据放入待发送队列。
// 返回 false 表示本帧没有进入队列，例如尚未连接、未订阅或长度非法。
bool audio_tx_service_enqueue(const uint8_t *opus_data, size_t len);

// 在业务主循环中周期调用。
// 每次最多发送少量帧，避免 BLE 发送长期占用主循环。
void audio_tx_service_process();

// 清空所有待发送帧，但不重置累计发送统计。
void audio_tx_service_reset_queue();

// 读取并清空最近一个统计周期的计数。
AudioTxStats audio_tx_service_take_stats();
