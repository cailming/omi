#include "audio_tx_service.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ble_service.h"
#include "esp_log.h"
#include "omiglass_config.h"

namespace {

static const char *TAG = "audio_tx";

struct AudioFrame {
    size_t len = 0;
    std::array<uint8_t, OMI_OPUS_OUTPUT_MAX_BYTES> data{};
};

std::array<AudioFrame, OMI_AUDIO_TX_QUEUE_FRAMES> s_queue{};
size_t s_read_index = 0;
size_t s_write_index = 0;
size_t s_queue_count = 0;
uint16_t s_packet_index = 0;

AudioTxStats s_stats{};
bool s_initialized = false;
bool s_warned_small_mtu = false;

void pop_front()
{
    if (s_queue_count == 0) {
        return;
    }

    s_read_index = (s_read_index + 1) % OMI_AUDIO_TX_QUEUE_FRAMES;
    --s_queue_count;
}

void push_frame(const uint8_t *data, size_t len)
{
    // 实时音频更看重“新”而不是“完整”。
    // 队列满时丢掉最旧的一帧，避免延迟持续增长。
    if (s_queue_count == OMI_AUDIO_TX_QUEUE_FRAMES) {
        pop_front();
        ++s_stats.dropped_queue_full;
    }

    AudioFrame &slot = s_queue[s_write_index];
    slot.len = len;
    std::memcpy(slot.data.data(), data, len);

    s_write_index = (s_write_index + 1) % OMI_AUDIO_TX_QUEUE_FRAMES;
    ++s_queue_count;
    ++s_stats.enqueued_frames;
}

} // namespace

esp_err_t audio_tx_service_init()
{
    s_read_index = 0;
    s_write_index = 0;
    s_queue_count = 0;
    s_packet_index = 0;
    s_stats = {};
    s_warned_small_mtu = false;
    s_initialized = true;

    ESP_LOGI(
        TAG,
        "BLE 音频发送队列初始化完成：%u 帧，单包最大 %u 字节",
        static_cast<unsigned>(OMI_AUDIO_TX_QUEUE_FRAMES),
        static_cast<unsigned>(OMI_AUDIO_TX_PACKET_MAX_BYTES));
    return ESP_OK;
}

bool audio_tx_service_enqueue(const uint8_t *opus_data, size_t len)
{
    if (!s_initialized || opus_data == nullptr || len == 0 || len > OMI_OPUS_OUTPUT_MAX_BYTES) {
        return false;
    }

    // 未连接或未订阅 Audio Data 时不积压旧语音。
    // 这样客户端刚打开 Notify 时收到的一定是“此刻”的音频，而不是数百毫秒前的缓存。
    if (!ble_service_is_connected() || !ble_service_audio_subscribed()) {
        ++s_stats.dropped_not_ready;
        return false;
    }

    push_frame(opus_data, len);
    return true;
}

void audio_tx_service_process()
{
    if (!s_initialized) {
        return;
    }

    if (!ble_service_is_connected() || !ble_service_audio_subscribed()) {
        // 一旦连接或订阅失效，立即清空待发数据，防止重连后发送陈旧语音。
        audio_tx_service_reset_queue();
        return;
    }

    size_t sent_this_round = 0;

    while (s_queue_count > 0 && sent_this_round < OMI_AUDIO_TX_MAX_PACKETS_PER_PROCESS) {
        const AudioFrame &frame = s_queue[s_read_index];
        const size_t packet_len = OMI_AUDIO_PACKET_HEADER_SIZE + frame.len;

        // ATT Notification 的 Attribute Value 最大长度为 MTU - 3。
        // 如果当前手机尚未把 MTU 协商到足够大小，本帧直接丢弃，优先保持实时性。
        if (ble_service_mtu() < packet_len + 3) {
            ++s_stats.dropped_mtu_too_small;
            pop_front();

            if (!s_warned_small_mtu) {
                ESP_LOGW(
                    TAG,
                    "当前 ATT MTU=%u，无法容纳 %u 字节音频包；等待手机完成 MTU 协商",
                    ble_service_mtu(),
                    static_cast<unsigned>(packet_len));
                s_warned_small_mtu = true;
            }
            continue;
        }

        s_warned_small_mtu = false;

        std::array<uint8_t, OMI_AUDIO_TX_PACKET_MAX_BYTES> packet{};
        packet[0] = static_cast<uint8_t>(s_packet_index & 0xFF);
        packet[1] = static_cast<uint8_t>((s_packet_index >> 8) & 0xFF);
        packet[2] = OMI_AUDIO_SUB_INDEX;
        std::memcpy(packet.data() + OMI_AUDIO_PACKET_HEADER_SIZE, frame.data.data(), frame.len);

        const esp_err_t err = ble_service_notify_audio(packet.data(), packet_len);
        if (err == ESP_OK) {
            pop_front();
            ++s_packet_index;
            ++s_stats.sent_frames;
            ++sent_this_round;
            continue;
        }

        // BLE 发送失败时不反复重试旧帧。
        // 对实时语音而言，持续重试会把排队延迟越拖越长，因此丢掉本帧并等待下一帧。
        pop_front();
        ++s_stats.notify_errors;
        ESP_LOGW(TAG, "Audio Data notify 失败: %s", esp_err_to_name(err));
        break;
    }
}

bool audio_tx_service_has_pending()
{
    return s_initialized && s_queue_count > 0;
}

void audio_tx_service_reset_queue()
{
    s_read_index = 0;
    s_write_index = 0;
    s_queue_count = 0;
}

AudioTxStats audio_tx_service_take_stats()
{
    AudioTxStats result = s_stats;
    result.queue_depth = s_queue_count;

    // queue_depth 是瞬时状态，其他字段作为“自上次读取以来”的区间统计清零。
    s_stats = {};
    return result;
}
