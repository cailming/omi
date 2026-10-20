#include "board_service.h"
#include "mic_service.h"
#include "omiglass_config.h"

#include <cstddef>
#include <cstdint>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "omiglass";

namespace {

uint64_t s_mic_samples_since_report = 0;
int32_t s_mic_peak_since_report = 0;

void on_mic_pcm(int16_t *data, size_t samples)
{
    s_mic_samples_since_report += samples;

    // Phase 2 暂时只做轻量统计，不保存音频。
    // Phase 3 会把这里替换为 Opus 编码器的 PCM 输入接口。
    for (size_t i = 0; i < samples; ++i) {
        int32_t sample = static_cast<int32_t>(data[i]);
        if (sample < 0) {
            sample = -sample;
        }
        if (sample > s_mic_peak_since_report) {
            s_mic_peak_since_report = sample;
        }
    }
}

void report_mic_status_if_needed(uint64_t now_ms, uint64_t &last_report_ms)
{
    if (now_ms - last_report_ms < 1000) {
        return;
    }

    if (mic_service_is_running()) {
        ESP_LOGI(
            TAG,
            "麦克风采集状态：最近约 1 秒收到 %llu samples，峰值=%ld",
            static_cast<unsigned long long>(s_mic_samples_since_report),
            static_cast<long>(s_mic_peak_since_report));
    }

    s_mic_samples_since_report = 0;
    s_mic_peak_since_report = 0;
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
    ESP_LOGI(TAG, "CPU 核心数: %d，芯片修订版本: %d", chip_info.cores, chip_info.revision);

    const esp_err_t board_err = board_service_init();
    if (board_err != ESP_OK) {
        ESP_LOGE(TAG, "板级基础服务初始化失败，错误码: %s", esp_err_to_name(board_err));
        return;
    }

    ESP_LOGI(TAG, "设备序列号: %s", board_device_serial());
    ESP_LOGI(TAG, "Phase 1 基础板级服务已启动");

    // 先注册 PCM 回调，再启动 PDM RX。
    mic_service_set_callback(on_mic_pcm);
    const esp_err_t mic_err = mic_service_start();
    if (mic_err == ESP_OK) {
        ESP_LOGI(TAG, "Phase 2 PDM 麦克风已启动，等待 PCM 数据");
    } else {
        // 麦克风失败时不让整个固件退出，仍保留按键和 LED，方便现场诊断。
        ESP_LOGE(TAG, "PDM 麦克风启动失败: %s", esp_err_to_name(mic_err));
    }

    uint64_t last_mic_report_ms = board_millis();

    while (true) {
        board_service_process();

        if (mic_service_is_running()) {
            // 每次读取 320 samples，即 16kHz 下约 20ms PCM。
            mic_service_process();
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
        report_mic_status_if_needed(now_ms, last_mic_report_ms);

        // I2S 读取本身会阻塞到一帧完成；这里再短暂让出 CPU，避免异常情况下空转。
        board_delay_ms(1);
    }
}
