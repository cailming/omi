#include "audio_tx_service.h"
#include "battery_service.h"
#include "ble_service.h"
#include "board_service.h"
#include "camera_service.h"
#include "mic_service.h"
#include "omiglass_config.h"
#include "opus_service.h"
#include "ota_service.h"
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

bool s_ota_paused_microphone = false;
bool s_pending_long_press_shutdown = false;

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

    audio_tx_service_enqueue(data, len);
}

void on_ota_status(uint8_t status, uint8_t progress)
{
    // OTA Service 不直接依赖 BLE；由 app_main 把状态桥接回原 OTA Data Characteristic。
    const esp_err_t err = ble_service_update_ota_status(status, progress, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "更新 BLE OTA 状态失败: %s", esp_err_to_name(err));
    }
}

void on_ble_ota_command(const uint8_t *data, size_t len)
{
    // 该回调运行在 NimBLE Host Task 中，因此这里只做快速解析/启动 OTA Task。
    ota_service_handle_command(data, len);
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
    ESP_LOGI(TAG, "Deep Sleep 前释放 OTA / Audio / Photo / Camera / Battery 资源");

    // Power Service 只会在 OTA Task 已安全退出后进入这里，因此可以释放 Wi-Fi Driver。
    ota_service_deinit();

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

void update_ota_maintenance_mode(bool ota_busy, bool &last_ota_busy)
{
    if (ota_busy && !last_ota_busy) {
        ESP_LOGI(TAG, "进入 OTA 维护模式：暂停 Mic / BLE Audio / Photo 数据面，优先保证 HTTPS OTA");

        audio_tx_service_reset_queue();
        photo_service_cancel_upload();

        if (mic_service_is_running()) {
            mic_service_stop();
            s_ota_paused_microphone = true;
        }
    } else if (!ota_busy && last_ota_busy) {
        const BatteryReading latest_battery = battery_service_latest();
        const bool shutdown_pending =
            s_pending_long_press_shutdown ||
            power_service_state() == PowerState::POWERING_OFF ||
            (latest_battery.valid && latest_battery.battery_voltage <= OMI_BATTERY_CRITICAL_VOLTAGE);

        if (shutdown_pending) {
            // OTA 是因为关机/临界低电被取消时，不先恢复高功耗的数据面；下一步直接进入 Power Service 关机流程。
            // 同时检查 PowerState，覆盖 OTA Task 恰好在同一主循环两次 busy 读取之间退出的竞态窗口。
            ESP_LOGI(TAG, "OTA 已安全退出且存在待处理关机条件，不恢复 Mic / Audio / Photo");
            s_ota_paused_microphone = false;
        } else {
            ESP_LOGI(TAG, "退出 OTA 维护模式：恢复实时业务");

            if (s_ota_paused_microphone) {
                const esp_err_t err = mic_service_start();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "OTA 结束后恢复 PDM 麦克风失败: %s", esp_err_to_name(err));
                }
                s_ota_paused_microphone = false;
            }
        }
    }

    last_ota_busy = ota_busy;
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

    if (opus_service_is_ready() && !ota_service_is_busy()) {
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
            "Power：state=%s battery=%.3fV/%u%% BLE=%s Audio=%s Photo=%s OTA=%s(0x%02X/%u%%)",
            power_service_state_name(),
            static_cast<double>(battery.battery_voltage),
            battery.percentage,
            ble_service_is_connected() ? "连接" : "未连接",
            ble_service_audio_subscribed() ? "活动" : "空闲",
            photo_service_is_uploading() ? "上传中" : "空闲",
            ota_service_is_busy() ? "执行中" : "空闲",
            ota_service_status(),
            ota_service_progress());
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

    power_service_set_prepare_deep_sleep_handler(prepare_for_deep_sleep);
    const esp_err_t power_err = power_service_init();
    if (power_err == ESP_OK) {
        ESP_LOGI(TAG, "Phase 9 Power Management 已启动");
    } else {
        ESP_LOGE(TAG, "Power Management 初始化失败: %s", esp_err_to_name(power_err));
    }

    // Phase 10：先初始化 OTA 协议和网络事件基础设施，但保持 Wi-Fi 关闭。
    const esp_err_t ota_err = ota_service_init();
    if (ota_err == ESP_OK) {
        ota_service_set_status_handler(on_ota_status);
        ble_service_set_ota_command_handler(on_ble_ota_command);
        ESP_LOGI(TAG, "Phase 10 Wi-Fi / HTTPS OTA 已启动，等待 BLE OTA 命令");
    } else {
        ESP_LOGE(TAG, "OTA Service 初始化失败: %s", esp_err_to_name(ota_err));
    }

    const esp_err_t ble_err = ble_service_init(board_device_serial());
    if (ble_err == ESP_OK) {
        ESP_LOGI(TAG, "Phase 4 BLE Core 已启动");
        // 确保 OTA Control 初始 Read 值与 OTA Service 一致。
        ble_service_update_ota_status(ota_service_status(), ota_service_progress(), false);
    } else {
        ESP_LOGE(TAG, "BLE Core 初始化失败: %s", esp_err_to_name(ble_err));
    }

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
    bool last_ota_busy = false;

    while (true) {
        board_service_set_connected(ble_service_is_connected());
        board_service_process();

        bool ota_busy = ota_service_is_busy();
        update_ota_maintenance_mode(ota_busy, last_ota_busy);

        // OTA 维护模式下暂停实时音频和图片数据面，BLE OTA 状态通知仍由 NimBLE Task 正常发送。
        if (!ota_busy) {
            if (mic_service_is_running()) {
                mic_service_process();
            }

            if (opus_service_is_ready()) {
                opus_service_process();
            }

            // 实时音频始终优先于图片。
            audio_tx_service_process();
        }

        const uint64_t now_ms = board_millis();
        if (!ota_busy) {
            photo_service_process(now_ms);
        }

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
            if (ota_busy) {
                // OTA 写 Flash 时不直接睡眠：先取消 OTA，等任务安全退出后再把长按事件交给 Power Service。
                ESP_LOGW(TAG, "OTA 执行中收到长按关机：先取消 OTA，再进入 Deep Sleep");
                ota_service_cancel();
                s_pending_long_press_shutdown = true;
            } else {
                ESP_LOGI(TAG, "收到长按事件，交给 Phase 9 执行关机 / Deep Sleep");
            }
        }

        const BatteryReading latest_battery = battery_service_latest();
        ota_busy = ota_service_is_busy();

        // 临界低电也必须先让 OTA Task 停在安全点，避免写 Flash 中途 Deep Sleep。
        if (ota_busy && latest_battery.valid &&
            latest_battery.battery_voltage <= OMI_BATTERY_CRITICAL_VOLTAGE) {
            ESP_LOGW(TAG, "OTA 执行期间达到临界低电，先取消 OTA 再执行低电保护关机");
            ota_service_cancel();
        }

        bool deliver_long_press = event == BoardButtonEvent::LONG_PRESS && !ota_busy;
        if (s_pending_long_press_shutdown && !ota_busy) {
            deliver_long_press = true;
            s_pending_long_press_shutdown = false;
        }

        // Phase 9：统一处理 CPU 频率、自动 Light Sleep、低电策略和 Deep Sleep。
        PowerServiceInputs power_inputs{};
        power_inputs.ble_connected = ble_service_is_connected();
        // OTA 期间借用“高带宽活动”约束：保持 CPU Max / No-Light-Sleep，且阻止离线空闲 Deep Sleep。
        power_inputs.audio_streaming = (ble_service_audio_subscribed() && !ota_busy) || ota_busy;
        power_inputs.photo_uploading = photo_service_is_uploading() && !ota_busy;
        // OTA Task 尚未退出时暂缓临界低电 Deep Sleep；任务退出后下一轮立即恢复真实 Battery 判定。
        power_inputs.battery_valid = latest_battery.valid && !ota_busy;
        power_inputs.battery_voltage = latest_battery.battery_voltage;
        power_inputs.short_press = event == BoardButtonEvent::SHORT_PRESS;
        power_inputs.long_press = deliver_long_press;
        power_service_process(now_ms, power_inputs);

        report_runtime_status_if_needed(now_ms, last_runtime_report_ms);

        if (ota_busy) {
            // OTA Task 自己阻塞在 Wi-Fi/TLS I/O；主业务循环只负责 BLE/Power/Battery 协调。
            board_delay_ms(20);
        } else if (ble_service_audio_subscribed() || photo_service_is_uploading()) {
            board_delay_ms(1);
        } else {
            board_delay_ms(20);
        }
    }
}
