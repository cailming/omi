#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"

static const char *TAG = "omiglass";

extern "C" void app_main(void)
{
    esp_chip_info_t chip_info{};
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "OmiGlass native ESP-IDF migration firmware booting");
    ESP_LOGI(TAG, "Target cores: %d, revision: %d", chip_info.cores, chip_info.revision);
    ESP_LOGI(TAG, "Phase 0 skeleton only; Arduino firmware remains the reference implementation");
}
