#include "board_service.h"
#include "omiglass_config.h"

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "omiglass";

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

    while (true) {
        board_service_process();

        const BoardButtonEvent event = board_service_take_button_event();
        if (event == BoardButtonEvent::SHORT_PRESS) {
            // 目前只验证按键事件链路。后续会把短按行为接回业务状态机。
            ESP_LOGI(TAG, "收到短按事件");
        } else if (event == BoardButtonEvent::LONG_PRESS) {
            // 深睡关机将在电源管理阶段迁移，这里先确认 2 秒长按可以被稳定识别。
            ESP_LOGI(TAG, "收到长按事件，等待后续电源管理模块处理");
        }

        // 10ms 主循环足以支撑当前按键/LED 阶段，同时避免空转占满 CPU。
        board_delay_ms(10);
    }
}
