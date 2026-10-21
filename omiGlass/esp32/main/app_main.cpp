#include "ble_service.h"
#include "board_service.h"
#include "mic_service.h"
#include "omiglass_config.h"
#include "opus_service.h"

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
    // Opus 服务允许任意长度输入，不依赖 I2S 恰好一次返回 320 samples。
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

    // Phase 4 明确不发送实时音频。
    // Phase 5 才会把这里的 Opus 帧写入 BLE 音频发送队列并执行 notify。
    ++s_opus_frames_since_report;
    s_opus_bytes_since_report += len;
    if (len > s_opus_max_frame_bytes) {
        s_opus_max_frame_bytes = len;
    }
}

void report_audio_status_if_needed(uint64_t now_ms, uint64_t &last_report_ms)
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

    // Phase 4：初始化 ESP-IDF 原生 NimBLE GATT Server。
    // BLE 初始化失败不会阻止 Mic / Opus 继续启动，便于各阶段独立诊断。
    const esp_err_t ble_err = ble_service_init(board_device_serial());
    if (ble_err == ESP_OK) {
        ESP_LOGI(TAG, "Phase 4 BLE Core 已启动；本阶段不发送实时音频");
    } else {
        ESP_LOGE(TAG, "BLE Core 初始化失败: %s", esp_err_to_name(ble_err));
    }

    // 先初始化 Opus，再启动麦克风。这样第一批 PCM 到达时就可以直接进入编码环形缓冲区。
    opus_service_set_callback(on_opus_encoded);
    const esp_err_t opus_err = opus_service_init();
    if (opus_err == ESP_OK) {
        ESP_LOGI(TAG, "Phase 3 Opus 编码器已启动，Codec ID=%u", opus_service_codec_id());
    } else {
        // Opus 初始化失败时仍继续启动麦克风，便于独立诊断 Phase 2 硬件链路。
        ESP_LOGE(TAG, "Opus 编码器初始化失败: %s", esp_err_to_name(opus_err));
    }

    // 注册 PCM 回调后启动 PDM RX。
    mic_service_set_callback(on_mic_pcm);
    const esp_err_t mic_err = mic_service_start();
    if (mic_err == ESP_OK) {
        ESP_LOGI(TAG, "Phase 2 PDM 麦克风已启动，等待 PCM 数据");
    } else {
        // 麦克风失败时不让整个固件退出，仍保留按键、LED 和 BLE，方便现场诊断。
        ESP_LOGE(TAG, "PDM 麦克风启动失败: %s", esp_err_to_name(mic_err));
    }

    uint64_t last_audio_report_ms = board_millis();

    while (true) {
        // BLE Host 自己运行在 NimBLE FreeRTOS Task 中，主循环只同步连接状态给 LED。
        board_service_set_connected(ble_service_is_connected());
        board_service_process();

        if (mic_service_is_running()) {
            // 每次请求 320 samples，即 16kHz 下约 20ms PCM。
            mic_service_process();
        }

        if (opus_service_is_ready()) {
            // 与原 Arduino 固件一致：采集和编码分成两个步骤，中间通过 PCM 环形缓冲解耦。
            opus_service_process();
        }

        const BoardButtonEvent event = board_service_take_button_event();
        if (event == BoardButtonEvent::SHORT_PRESS) {
            // 目前只验证按键事件链路。后续会把短按行为接回业务状态机。
            ESP_LOGI(TAG, "收到短按事件");
        } else if (event == BoardButtonEvent::LONG_PRESS) {
            // 深睡关机将在电源管理阶段迁移，这里先确认 2 秒长按可以被稳定识别。
            ESP_LOGI(TAG, "收到长按事件，等待后续电源管理模块处理");
        }

        const uint64_t now_ms = board_millis();
        report_audio_status_if_needed(now_ms, last_audio_report_ms);

        // I2S 读取本身会等待一帧；这里再短暂让出 CPU，避免异常情况下空转。
        board_delay_ms(1);
    }
}
