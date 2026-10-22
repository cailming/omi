#include "audio_tx_service.h"
#include "battery_service.h"
#include "ble_service.h"
#include "board_service.h"
#include "camera_service.h"
#include "mic_service.h"
#include "omiglass_config.h"
#include "opus_service.h"
#include "photo_service.h"
#include "power_service.h"

#include <cstddef>
#include <cstdint>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "omiglass";

namespace {

uint64_t s_mic_samples_since_report = 0;
int32_t s_mic_peak_since_report = 0;
uint64_t s_opus_frames_since_report = 0;
uint64_t s_opus_bytes_since_report = 0;
size_t s_opus_max_frame_bytes = 0;

void on_mic_pcm(int16_t *data, size_t samples)
{
    s_mic_samples_since_report += samples;

    // 保留 PCM 峰值统计，便于烧板后快速判断麦克风输入是否正常。
    for (size_t i = 0; i < samples; ++i) {
        int32_t sample = static_cast<int32_t>(data[i]);
        if (sample < 0) {
            sample = -sample;
        }
        if (sample > s_mic_peak_since_report) {
            s_mic_peak_since_report = sample;
        }
    }

    // 将麦克风 PCM 送入 Opus 环形缓冲区。
    if (opus_service_is_ready()) {
        const esp_err_t err = opus_service_receive_pcm(data, samples);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "向 Opus 提交 PCM 失败: %s", esp_err_to_name(err));
        }
    }
}

void on_opus_encoded(const uint8_t *data, size_t len)
{
    if (data == nullptr || len == 0) {
        return;
    }

    ++s_opus_frames_since_report;
    s_opus_bytes_since_report += len;
    if (len > s_opus_max_frame_bytes) {
        s_opus_max_frame_bytes = len;
    }

    // Phase 5：编码完成后将 Opus 帧交给 BLE 音频发送队列。
    audio_tx_service_enqueue(data, len);
}

void publish_battery_reading(bool notify_if_subscribed)
{
    if (!battery_service_is_ready()) {
        return;
    }

    BatteryReading reading{};
    const esp_err_t err = battery_service_read(&reading);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Battery 检测失败: %s", esp_err_to_name(err));
        return;
    }

    const esp_err_t ble_err = ble_service_update_battery_level(reading.percentage, notify_if_subscribed);
    if (ble_err != ESP_OK) {
        ESP_LOGW(TAG, "更新 BLE Battery Level 失败: %s", esp_err_to_name(ble_err));
    }
}

void prepare_for_deep_sleep()
{
    ESP_LOGI(TAG, "Deep Sleep 前释放 Audio / Photo / Camera / Battery 资源");

    // 先停止上层业务，再释放其依赖的硬件资源。
    audio_tx_service_reset_queue();
    photo_service_cancel_upload();

    if (mic_service_is_running()) {
        mic_service_stop();
    }
    if (opus_service_is_ready()) {
        opus_service_deinit();
    }
    if (camera_service_is_ready()) {
        camera_service_deinit();
    }
    if (battery_service_is_ready()) {
        battery_service_deinit();
    }

    // NimBLE Host / Controller 由 power_service 在进入 Deep Sleep 前统一停止。
}

void report_runtime_status_if_needed(uint64_t now_ms, uint64_t &last_report_ms)
{
    if (now_ms - last_report_ms < 1000) {
        return;
    }

    if (mic_service_is_running()) {
        ESP_LOGI(
            TAG,
            "麦克风：最近约 1 秒收到 %llu samples，峰值=%ld",
            static_cast<unsigned long long>(s_mic_samples_since_report),
            static_cast<long>(s_mic_peak_since_report));
    }

    if (opus_service_is_ready()) {
        const uint64_t average_frame_bytes =
            s_opus_frames_since_report > 0 ? s_opus_bytes_since_report / s_opus_frames_since_report : 0;

        ESP_LOGI(
            TAG,
            "Opus：最近约 1 秒编码 %llu 帧，共 %llu 字节，平均 %llu 字节/帧，最大 %u 字节",
            static_cast<unsigned long long>(s_opus_frames_since_report),
            static_cast<unsigned long long>(s_opus_bytes_since_report),
            static_cast<unsigned long long>(average_frame_bytes),
            static_cast<unsigned>(s_opus_max_frame_bytes));
    }

    const AudioTxStats tx_stats = audio_tx_service_take_stats();
    if (ble_service_is_connected() || tx_stats.enqueued_frames > 0 || tx_stats.sent_frames > 0 ||
        tx_stats.dropped_queue_full > 0 || tx_stats.dropped_mtu_too_small > 0 || tx_stats.notify_errors > 0) {
        ESP_LOGI(
            TAG,
            "BLE Audio：订阅=%s MTU=%u 入队=%lu 发送=%lu 未就绪跳过=%lu 队列满丢帧=%lu MTU丢帧=%lu notify错误=%lu 当前队列=%u",
            ble_service_audio_subscribed() ? "是" : "否",
            ble_service_mtu(),
            static_cast<unsigned long>(tx_stats.enqueued_frames),
            static_cast<unsigned long>(tx_stats.sent_frames),
            static_cast<unsigned long>(tx_stats.dropped_not_ready),
            static_cast<unsigned long>(tx_stats.dropped_queue_full),
            static_cast<unsigned long>(tx_stats.dropped_mtu_too_small),
            static_cast<unsigned long>(tx_stats.notify_errors),
            static_cast<unsigned>(tx_stats.queue_depth));
    }

    const PhotoServiceStats photo_stats = photo_service_take_stats();
    if (photo_stats.captures_started > 0 || photo_stats.captures_completed > 0 || photo_stats.capture_errors > 0 ||
        photo_stats.uploads_cancelled > 0 || photo_stats.chunks_sent > 0 || photo_stats.notify_errors > 0 ||
        photo_stats.uploading) {
        ESP_LOGI(
            TAG,
            "BLE Photo：订阅=%s 开始=%lu 完成=%lu 抓图错误=%lu 取消=%lu chunk=%lu JPEG字节=%lu 音频让路=%lu MTU等待=%lu notify错误=%lu 上传中=%s 剩余=%u",
            ble_service_photo_subscribed() ? "是" : "否",
            static_cast<unsigned long>(photo_stats.captures_started),
            static_cast<unsigned long>(photo_stats.captures_completed),
            static_cast<unsigned long>(photo_stats.capture_errors),
            static_cast<unsigned long>(photo_stats.uploads_cancelled),
            static_cast<unsigned long>(photo_stats.chunks_sent),
            static_cast<unsigned long>(photo_stats.jpeg_bytes_sent),
            static_cast<unsigned long>(photo_stats.yielded_to_audio),
            static_cast<unsigned long>(photo_stats.mtu_waits),
            static_cast<unsigned long>(photo_stats.notify_errors),
            photo_stats.uploading ? "是" : "否",
            static_cast<unsigned>(photo_stats.remaining_bytes));
    }

    const BatteryReading battery = battery_service_latest();
    if (battery.valid) {
        ESP_LOGI(
            TAG,
            "Power：state=%s battery=%.3fV/%u%% BLE=%s Audio=%s Photo=%s",
            power_service_state_name(),
            static_cast<double>(battery.battery_voltage),
            battery.percentage,
            ble_service_is_connected() ? "连接" : "未连接",
            ble_service_audio_subscribed() ? "活动" : "空闲",
            photo_service_is_uploading() ? "上传中" : "空闲");
    }

    s_mic_samples_since_report = 0;
    s_mic_peak_since_report = 0;
    s_opus_frames_since_report = 0;
    s_opus_bytes_since_report = 0;
    s_opus_max_frame_bytes = 0;
    last_report_ms = now_ms;
}

} // namespace

extern "C" void app_main(void)
{
    esp_chip_info_t chip_info{};
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "OmiGlass 原生 ESP-IDF 固件启动");
    ESP_LOGI(TAG, "固件版本: %s", OMI_FIRMWARE_VERSION_STRING);
    ESP_LOGI(TAG, "硬件版本: %s", OMI_HARDWARE_REVISION);
    ESP_LOGI(TAG, "ESP-IDF 迁移基线: v6.1.x");
    ESP_LOGI(TAG, "CPU 核心数: %d，芯片修订版本: %d", chip_info.cores, chip_info.revision);

    const esp_err_t board_err = board_service_init();
    if (board_err != ESP_OK) {
        ESP_LOGE(TAG, "板级基础服务初始化失败，错误码: %s", esp_err_to_name(board_err));
        return;
    }

    ESP_LOGI(TAG, "设备序列号: %s", board_device_serial());
    ESP_LOGI(TAG, "Phase 1 基础板级服务已启动");

    // Phase 9：先启用 ESP-IDF 原生 Power Management。
    // 这样后续 BLE / Camera / I2S 初始化过程也在 80MHz 上限下运行。
    power_service_set_prepare_deep_sleep_handler(prepare_for_deep_sleep);
    const esp_err_t power_err = power_service_init();
    if (power_err == ESP_OK) {
        ESP_LOGI(TAG, "Phase 9 Power Management 已启动");
    } else {
        ESP_LOGE(TAG, "Power Management 初始化失败: %s", esp_err_to_name(power_err));
    }

    // 初始化 ESP-IDF 原生 NimBLE GATT Server。
    const esp_err_t ble_err = ble_service_init(board_device_serial());
    if (ble_err == ESP_OK) {
        ESP_LOGI(TAG, "Phase 4 BLE Core 已启动");
    } else {
        ESP_LOGE(TAG, "BLE Core 初始化失败: %s", esp_err_to_name(ble_err));
    }

    // Phase 8：初始化 GPIO2 Battery ADC，并立即做第一次检测。
    const esp_err_t battery_err = battery_service_init();
    if (battery_err == ESP_OK) {
        ESP_LOGI(TAG, "Phase 8 Battery ADC 已启动");
        publish_battery_reading(false);
    } else {
        ESP_LOGE(TAG, "Battery ADC 初始化失败: %s", esp_err_to_name(battery_err));
    }

    const esp_err_t tx_err = audio_tx_service_init();
    if (tx_err == ESP_OK) {
        ESP_LOGI(TAG, "Phase 5 BLE Audio TX 已启动，等待手机订阅 Audio Data Notify");
    } else {
        ESP_LOGE(TAG, "BLE Audio TX 初始化失败: %s", esp_err_to_name(tx_err));
    }

    const esp_err_t camera_err = camera_service_init();
    if (camera_err == ESP_OK) {
        ESP_LOGI(TAG, "Phase 6 Camera 已启动");
    } else {
        ESP_LOGE(TAG, "Camera 初始化失败: %s", esp_err_to_name(camera_err));
    }

    const esp_err_t photo_err = photo_service_init();
    if (photo_err == ESP_OK) {
        ESP_LOGI(TAG, "Phase 7 Photo BLE 已启动，等待 Photo Data Notify 订阅");
    } else {
        ESP_LOGE(TAG, "Photo BLE 初始化失败: %s", esp_err_to_name(photo_err));
    }

    opus_service_set_callback(on_opus_encoded);
    const esp_err_t opus_err = opus_service_init();
    if (opus_err == ESP_OK) {
        ESP_LOGI(TAG, "Phase 3 Opus 编码器已启动，Codec ID=%u", opus_service_codec_id());
    } else {
        ESP_LOGE(TAG, "Opus 编码器初始化失败: %s", esp_err_to_name(opus_err));
    }

    mic_service_set_callback(on_mic_pcm);
    const esp_err_t mic_err = mic_service_start();
    if (mic_err == ESP_OK) {
        ESP_LOGI(TAG, "Phase 2 PDM 麦克风已启动，等待 PCM 数据");
    } else {
        ESP_LOGE(TAG, "PDM 麦克风启动失败: %s", esp_err_to_name(mic_err));
    }

    uint64_t last_runtime_report_ms = board_millis();
    uint64_t last_battery_check_ms = board_millis();
    bool last_battery_subscribed = false;

    while (true) {
        board_service_set_connected(ble_service_is_connected());
        board_service_process();

        if (mic_service_is_running()) {
            mic_service_process();
        }

        if (opus_service_is_ready()) {
            opus_service_process();
        }

        // 实时音频始终优先于图片。
        audio_tx_service_process();

        const uint64_t now_ms = board_millis();
        photo_service_process(now_ms);

        if (battery_service_is_ready() && now_ms - last_battery_check_ms >= OMI_BATTERY_CHECK_INTERVAL_MS) {
            publish_battery_reading(true);
            last_battery_check_ms = now_ms;
        }

        // 手机刚订阅 Battery Notify 时立即推送最近一次有效结果。
        const bool battery_subscribed = ble_service_battery_subscribed();
        if (battery_service_is_ready() && battery_subscribed && !last_battery_subscribed) {
            const BatteryReading latest = battery_service_latest();
            if (latest.valid) {
                const esp_err_t notify_err = ble_service_update_battery_level(latest.percentage, true);
                if (notify_err != ESP_OK) {
                    ESP_LOGW(TAG, "首次 Battery Notify 失败: %s", esp_err_to_name(notify_err));
                }
            }
        }
        last_battery_subscribed = battery_subscribed;

        const BoardButtonEvent event = board_service_take_button_event();
        if (event == BoardButtonEvent::SHORT_PRESS) {
            ESP_LOGI(TAG, "收到短按事件，登记为用户活动");
        } else if (event == BoardButtonEvent::LONG_PRESS) {
            ESP_LOGI(TAG, "收到长按事件，交给 Phase 9 执行关机 / Deep Sleep");
        }

        // Phase 9：统一处理 CPU 频率、自动 Light Sleep、低电策略和 Deep Sleep。
        const BatteryReading latest_battery = battery_service_latest();
        PowerServiceInputs power_inputs{};
        power_inputs.ble_connected = ble_service_is_connected();
        power_inputs.audio_streaming = ble_service_audio_subscribed();
        power_inputs.photo_uploading = photo_service_is_uploading();
        power_inputs.battery_valid = latest_battery.valid;
        power_inputs.battery_voltage = latest_battery.battery_voltage;
        power_inputs.short_press = event == BoardButtonEvent::SHORT_PRESS;
        power_inputs.long_press = event == BoardButtonEvent::LONG_PRESS;
        power_service_process(now_ms, power_inputs);

        report_runtime_status_if_needed(now_ms, last_runtime_report_ms);

        // 高带宽业务保持快速调度；空闲时放大阻塞窗口，给 Tickless Idle / 自动 Light Sleep 更多机会。
        if (ble_service_audio_subscribed() || photo_service_is_uploading()) {
            board_delay_ms(1);
        } else {
            board_delay_ms(20);
        }
    }
}
