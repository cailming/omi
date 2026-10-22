#include "photo_service.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "audio_tx_service.h"
#include "ble_service.h"
#include "camera_service.h"
#include "esp_log.h"
#include "omiglass_config.h"

namespace {

static const char *TAG = "photo";

bool s_initialized = false;
bool s_capture_enabled = true;
bool s_capture_immediate = true;
uint32_t s_capture_interval_ms = OMI_CAMERA_CAPTURE_INTERVAL_MS;
uint64_t s_last_capture_ms = 0;

bool s_uploading = false;
CameraFrameView s_frame{};
size_t s_sent_bytes = 0;
uint16_t s_chunk_index = 0;

PhotoServiceStats s_stats{};
bool s_warned_small_mtu = false;

void reset_upload_state()
{
    s_uploading = false;
    s_frame = {};
    s_sent_bytes = 0;
    s_chunk_index = 0;
}

void release_frame_if_needed()
{
    if (camera_service_has_frame()) {
        camera_service_release_frame();
    }
}

void handle_photo_control(int8_t value)
{
    if (value == -1) {
        // 与原 Arduino 固件一致：-1 表示单拍。
        // 设为立即拍照，并在成功抓取后关闭后续自动拍摄。
        s_capture_enabled = true;
        s_capture_interval_ms = 0;
        s_capture_immediate = true;
        ESP_LOGI(TAG, "Photo Control：单拍");
        return;
    }

    if (value == 0) {
        // 停止后续拍照，但不强行中断当前已经开始的上传。
        s_capture_enabled = false;
        s_capture_interval_ms = 0;
        s_capture_immediate = false;
        ESP_LOGI(TAG, "Photo Control：停止后续拍照");
        return;
    }

    if (value >= 5) {
        // 原 Arduino 固件虽然接收一个数值参数，但最终仍固定使用配置中的 30 秒间隔。
        // 这里保持同样行为，避免迁移阶段改变手机端语义。
        s_capture_enabled = true;
        s_capture_interval_ms = OMI_CAMERA_CAPTURE_INTERVAL_MS;
        s_capture_immediate = true;
        ESP_LOGI(
            TAG,
            "Photo Control：启动间隔拍照，命令值=%d，实际固定间隔=%lu ms",
            static_cast<int>(value),
            static_cast<unsigned long>(s_capture_interval_ms));
        return;
    }

    ESP_LOGW(TAG, "忽略未定义的 Photo Control=%d", static_cast<int>(value));
}

bool capture_due(uint64_t now_ms)
{
    if (!s_capture_enabled || s_uploading) {
        return false;
    }

    if (s_capture_immediate) {
        return true;
    }

    if (s_capture_interval_ms == 0) {
        return false;
    }

    return now_ms - s_last_capture_ms >= s_capture_interval_ms;
}

void start_capture(uint64_t now_ms)
{
    CameraFrameView view{};
    const esp_err_t err = camera_service_capture(&view);
    if (err != ESP_OK) {
        ++s_stats.capture_errors;

        // 抓图失败后结束本次“立即触发”，避免主循环每 1ms 重试一次 Camera。
        // 周期模式等待下一个 30 秒周期；单拍模式等待用户再次发送命令。
        s_last_capture_ms = now_ms;
        s_capture_immediate = false;
        if (s_capture_interval_ms == 0) {
            s_capture_enabled = false;
        }

        ESP_LOGW(TAG, "Camera 抓图失败: %s", esp_err_to_name(err));
        return;
    }

    s_frame = view;
    s_sent_bytes = 0;
    s_chunk_index = 0;
    s_uploading = true;
    s_last_capture_ms = now_ms;
    s_capture_immediate = false;
    ++s_stats.captures_started;

    // 单拍模式在成功抓取后立即关闭后续自动拍摄。
    if (s_capture_interval_ms == 0) {
        s_capture_enabled = false;
    }

    ESP_LOGI(
        TAG,
        "开始图片上传：%ux%u JPEG=%u bytes orientation=%u",
        static_cast<unsigned>(s_frame.width),
        static_cast<unsigned>(s_frame.height),
        static_cast<unsigned>(s_frame.len),
        static_cast<unsigned>(s_frame.orientation));
}

bool send_data_chunk()
{
    if (s_sent_bytes >= s_frame.len) {
        return false;
    }

    std::array<uint8_t, OMI_PHOTO_PACKET_MAX_BYTES> packet{};
    const size_t remaining = s_frame.len - s_sent_bytes;
    size_t jpeg_bytes = 0;
    size_t packet_len = 0;

    if (s_chunk_index == 0) {
        // 首包：2 字节 frame index + 1 字节 orientation + JPEG 数据。
        packet[0] = 0;
        packet[1] = 0;
        packet[2] = s_frame.orientation;
        jpeg_bytes = std::min(remaining, OMI_PHOTO_FIRST_CHUNK_JPEG_BYTES);
        std::memcpy(packet.data() + OMI_PHOTO_FIRST_CHUNK_HEADER_BYTES, s_frame.data + s_sent_bytes, jpeg_bytes);
        packet_len = OMI_PHOTO_FIRST_CHUNK_HEADER_BYTES + jpeg_bytes;
    } else {
        // 后续包：2 字节 little-endian frame index + JPEG 数据。
        packet[0] = static_cast<uint8_t>(s_chunk_index & 0xFF);
        packet[1] = static_cast<uint8_t>((s_chunk_index >> 8) & 0xFF);
        jpeg_bytes = std::min(remaining, OMI_PHOTO_OTHER_CHUNK_JPEG_BYTES);
        std::memcpy(packet.data() + OMI_PHOTO_OTHER_CHUNK_HEADER_BYTES, s_frame.data + s_sent_bytes, jpeg_bytes);
        packet_len = OMI_PHOTO_OTHER_CHUNK_HEADER_BYTES + jpeg_bytes;
    }

    const esp_err_t err = ble_service_notify_photo(packet.data(), packet_len);
    if (err != ESP_OK) {
        ++s_stats.notify_errors;
        ESP_LOGW(TAG, "Photo Data notify 失败，chunk=%u: %s", s_chunk_index, esp_err_to_name(err));
        return false;
    }

    s_sent_bytes += jpeg_bytes;
    ++s_chunk_index;
    ++s_stats.chunks_sent;
    s_stats.jpeg_bytes_sent += static_cast<uint32_t>(jpeg_bytes);
    return true;
}

bool send_end_marker()
{
    const uint8_t end_marker[2] = {0xFF, 0xFF};
    const esp_err_t err = ble_service_notify_photo(end_marker, sizeof(end_marker));
    if (err != ESP_OK) {
        ++s_stats.notify_errors;
        ESP_LOGW(TAG, "Photo 结束标记 notify 失败: %s", esp_err_to_name(err));
        return false;
    }

    ++s_stats.chunks_sent;
    ++s_stats.captures_completed;

    ESP_LOGI(
        TAG,
        "图片上传完成：JPEG=%u bytes，数据分片=%u",
        static_cast<unsigned>(s_frame.len),
        static_cast<unsigned>(s_chunk_index));

    release_frame_if_needed();
    reset_upload_state();
    return true;
}

void process_upload()
{
    if (!s_uploading) {
        return;
    }

    if (!ble_service_is_connected() || !ble_service_photo_subscribed()) {
        ESP_LOGW(TAG, "BLE 连接或 Photo 订阅已失效，取消当前图片上传");
        photo_service_cancel_upload();
        return;
    }

    // 图片单包最大 202 字节，因此 ATT MTU 至少要达到 205。
    // 图片是完整性数据，MTU 未协商好时选择等待，不像实时音频那样直接丢帧。
    if (ble_service_mtu() < OMI_PHOTO_PACKET_MAX_BYTES + 3) {
        ++s_stats.mtu_waits;
        if (!s_warned_small_mtu) {
            ESP_LOGW(
                TAG,
                "当前 ATT MTU=%u，Photo 分包需要至少 %u；等待 MTU 协商",
                ble_service_mtu(),
                static_cast<unsigned>(OMI_PHOTO_PACKET_MAX_BYTES + 3));
            s_warned_small_mtu = true;
        }
        return;
    }
    s_warned_small_mtu = false;

    size_t chunks_this_round = 0;

    while (s_uploading && chunks_this_round < OMI_PHOTO_MAX_CHUNKS_PER_PROCESS) {
        // Audio 始终高于 Photo。只要音频队列还有待发数据，本轮立即让路。
        if (audio_tx_service_has_pending()) {
            ++s_stats.yielded_to_audio;
            return;
        }

        if (s_sent_bytes < s_frame.len) {
            if (!send_data_chunk()) {
                // 图片要求完整性，notify 失败时保留当前位置，下轮重试同一个 chunk。
                return;
            }
            ++chunks_this_round;
            continue;
        }

        // 所有 JPEG 数据发完后，再单独发送 0xFFFF 结束标记。
        send_end_marker();
        return;
    }
}

} // namespace

esp_err_t photo_service_init()
{
    s_capture_enabled = true;
    s_capture_immediate = true;
    s_capture_interval_ms = OMI_CAMERA_CAPTURE_INTERVAL_MS;
    s_last_capture_ms = 0;
    s_warned_small_mtu = false;
    s_stats = {};
    reset_upload_state();
    s_initialized = true;

    ESP_LOGI(
        TAG,
        "Photo BLE 服务初始化完成：首包 JPEG=%u bytes，后续=%u bytes，每轮最多 %u 个 chunk",
        static_cast<unsigned>(OMI_PHOTO_FIRST_CHUNK_JPEG_BYTES),
        static_cast<unsigned>(OMI_PHOTO_OTHER_CHUNK_JPEG_BYTES),
        static_cast<unsigned>(OMI_PHOTO_MAX_CHUNKS_PER_PROCESS));
    return ESP_OK;
}

void photo_service_process(uint64_t now_ms)
{
    if (!s_initialized) {
        return;
    }

    int8_t control = 0;
    if (ble_service_take_photo_control(&control)) {
        handle_photo_control(control);
    }

    if (s_uploading) {
        process_upload();
        return;
    }

    // 没有连接或客户端没有订阅 Photo Data 时不抓图，避免占住唯一 framebuffer 却无处发送。
    if (!camera_service_is_ready() || !ble_service_is_connected() || !ble_service_photo_subscribed()) {
        return;
    }

    if (!capture_due(now_ms)) {
        return;
    }

    start_capture(now_ms);

    // 抓图成功后本轮不立即连续发送，让主循环先回到 Audio TX，进一步保证音频优先级。
}

bool photo_service_is_uploading()
{
    return s_uploading;
}

void photo_service_cancel_upload()
{
    if (!s_uploading) {
        return;
    }

    release_frame_if_needed();
    reset_upload_state();
    ++s_stats.uploads_cancelled;
}

PhotoServiceStats photo_service_take_stats()
{
    PhotoServiceStats result = s_stats;
    result.uploading = s_uploading;
    result.remaining_bytes = s_uploading && s_frame.len >= s_sent_bytes ? s_frame.len - s_sent_bytes : 0;

    // uploading / remaining_bytes 是瞬时状态，其余计数按统计周期清零。
    s_stats = {};
    return result;
}
