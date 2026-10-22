#include "battery_service.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "omiglass_config.h"

namespace {

static const char *TAG = "battery";

adc_oneshot_unit_handle_t s_adc_handle = nullptr;
adc_cali_handle_t s_cali_handle = nullptr;
adc_unit_t s_adc_unit = ADC_UNIT_1;
adc_channel_t s_adc_channel = ADC_CHANNEL_0;

bool s_ready = false;
bool s_calibration_ready = false;
bool s_has_percentage = false;
uint8_t s_last_percentage = 0;
BatteryReading s_latest{};

uint8_t calculate_percentage(float battery_voltage)
{
    int percentage = 0;

    if (battery_voltage >= OMI_BATTERY_MAX_VOLTAGE) {
        percentage = 100;
    } else if (battery_voltage <= OMI_BATTERY_MIN_VOLTAGE) {
        percentage = 0;
    } else {
        const float range = OMI_BATTERY_MAX_VOLTAGE - OMI_BATTERY_MIN_VOLTAGE;
        percentage = static_cast<int>(
            ((battery_voltage - OMI_BATTERY_MIN_VOLTAGE) / range) * 100.0f);
    }

    percentage = std::clamp(percentage, 0, 100);

    // 保留原 Arduino 固件的平滑方式：
    // 如果一次检测与上次结果相差超过 5%，本次只向目标方向移动 2%。
    if (!s_has_percentage) {
        s_last_percentage = static_cast<uint8_t>(percentage);
        s_has_percentage = true;
        return s_last_percentage;
    }

    const int delta = percentage - static_cast<int>(s_last_percentage);
    if (delta > 5) {
        percentage = static_cast<int>(s_last_percentage) + 2;
    } else if (delta < -5) {
        percentage = static_cast<int>(s_last_percentage) - 2;
    }

    percentage = std::clamp(percentage, 0, 100);
    s_last_percentage = static_cast<uint8_t>(percentage);
    return s_last_percentage;
}

void delete_calibration()
{
    if (s_cali_handle == nullptr) {
        return;
    }

    const esp_err_t err = adc_cali_delete_scheme_curve_fitting(s_cali_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "释放 ADC Curve Fitting 校准失败: %s", esp_err_to_name(err));
    }

    s_cali_handle = nullptr;
    s_calibration_ready = false;
}

float battery_voltage_from_adc_millivolts(int adc_millivolts)
{
    return (static_cast<float>(adc_millivolts) / 1000.0f) * OMI_BATTERY_VOLTAGE_DIVIDER_RATIO;
}

} // namespace

esp_err_t battery_service_init()
{
    if (s_ready) {
        return ESP_OK;
    }

    // 不硬编码 ADC channel，而是让 ESP-IDF 根据 GPIO2 自动解析 Unit / Channel。
    esp_err_t err = adc_oneshot_io_to_channel(
        static_cast<int>(OMI_BATTERY_ADC_GPIO),
        &s_adc_unit,
        &s_adc_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d 不是有效 ADC 引脚: %s", OMI_BATTERY_ADC_GPIO, esp_err_to_name(err));
        return err;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg{};
    unit_cfg.unit_id = s_adc_unit;
    unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;

    err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建 ADC oneshot unit 失败: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t channel_cfg{};
    channel_cfg.bitwidth = ADC_BITWIDTH_12;

    // Arduino 版本使用最大衰减档位读取 GPIO2。
    // ESP-IDF v6.1 在 ESP32-S3 上对应使用 ADC_ATTEN_DB_12。
    channel_cfg.atten = ADC_ATTEN_DB_12;

    err = adc_oneshot_config_channel(s_adc_handle, s_adc_channel, &channel_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 Battery ADC channel 失败: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = nullptr;
        return err;
    }

    // ESP32-S3 支持 Curve Fitting 校准。
    // 如果极少数硬件校准创建失败，仍允许退回原 Arduino 风格的 raw->mV 近似换算，
    // 这样不会因为缺少校准数据让 Battery Service 整体失效。
    adc_cali_curve_fitting_config_t cali_cfg{};
    cali_cfg.unit_id = s_adc_unit;
    cali_cfg.chan = s_adc_channel;
    cali_cfg.atten = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_12;

    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (err == ESP_OK) {
        s_calibration_ready = true;
        ESP_LOGI(TAG, "Battery ADC Curve Fitting 校准已启用");
    } else {
        s_cali_handle = nullptr;
        s_calibration_ready = false;
        ESP_LOGW(
            TAG,
            "Battery ADC 校准不可用，将退回 raw/4095*3300 近似换算: %s",
            esp_err_to_name(err));
    }

    s_has_percentage = false;
    s_last_percentage = 0;
    s_latest = {};
    s_ready = true;

    ESP_LOGI(
        TAG,
        "Battery ADC 初始化完成：GPIO%d unit=%d channel=%d 分压比=%.3f",
        OMI_BATTERY_ADC_GPIO,
        static_cast<int>(s_adc_unit),
        static_cast<int>(s_adc_channel),
        static_cast<double>(OMI_BATTERY_VOLTAGE_DIVIDER_RATIO));
    return ESP_OK;
}

void battery_service_deinit()
{
    delete_calibration();

    if (s_adc_handle != nullptr) {
        const esp_err_t err = adc_oneshot_del_unit(s_adc_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "释放 ADC oneshot unit 失败: %s", esp_err_to_name(err));
        }
        s_adc_handle = nullptr;
    }

    s_ready = false;
    s_has_percentage = false;
    s_latest = {};
}

bool battery_service_is_ready()
{
    return s_ready;
}

esp_err_t battery_service_read(BatteryReading *reading)
{
    if (!s_ready || s_adc_handle == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (reading == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t raw_sum = 0;

    for (size_t i = 0; i < OMI_BATTERY_SAMPLE_COUNT; ++i) {
        int raw = 0;
        const esp_err_t err = adc_oneshot_read(s_adc_handle, s_adc_channel, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Battery ADC 第 %u 次采样失败: %s", static_cast<unsigned>(i + 1), esp_err_to_name(err));
            return err;
        }

        raw_sum += raw;

        if (i + 1 < OMI_BATTERY_SAMPLE_COUNT && OMI_BATTERY_SAMPLE_DELAY_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(OMI_BATTERY_SAMPLE_DELAY_MS));
        }
    }

    const int raw_average = static_cast<int>(raw_sum / static_cast<int64_t>(OMI_BATTERY_SAMPLE_COUNT));

    // Phase 11 同一批 raw sample 同时计算两条路径：
    // 1. Arduino 风格 raw/4095*3300；
    // 2. ESP-IDF Curve Fitting。
    // 最终业务仍优先使用校准结果，但日志会同时暴露两者，便于和万用表/旧固件做同板对比。
    const int raw_approx_millivolts =
        static_cast<int>((static_cast<int64_t>(raw_average) * 3300) / 4095);
    const float raw_approx_battery_voltage =
        battery_voltage_from_adc_millivolts(raw_approx_millivolts);

    int calibrated_millivolts = 0;
    float calibrated_battery_voltage = 0.0f;
    bool calibrated = false;

    if (s_calibration_ready && s_cali_handle != nullptr) {
        const esp_err_t cali_err =
            adc_cali_raw_to_voltage(s_cali_handle, raw_average, &calibrated_millivolts);
        if (cali_err == ESP_OK) {
            calibrated = true;
            calibrated_battery_voltage =
                battery_voltage_from_adc_millivolts(calibrated_millivolts);
        } else {
            ESP_LOGW(TAG, "ADC 校准换算失败，本次退回近似换算: %s", esp_err_to_name(cali_err));
        }
    }

    const int adc_millivolts = calibrated ? calibrated_millivolts : raw_approx_millivolts;
    float battery_voltage = calibrated ? calibrated_battery_voltage : raw_approx_battery_voltage;
    battery_voltage = std::clamp(
        battery_voltage,
        OMI_BATTERY_VOLTAGE_CLAMP_MIN,
        OMI_BATTERY_VOLTAGE_CLAMP_MAX);

    BatteryReading result{};
    result.raw_average = raw_average;
    result.adc_millivolts = adc_millivolts;
    result.battery_voltage = battery_voltage;
    result.raw_approx_millivolts = raw_approx_millivolts;
    result.raw_approx_battery_voltage = raw_approx_battery_voltage;
    result.calibrated_millivolts = calibrated_millivolts;
    result.calibrated_battery_voltage = calibrated_battery_voltage;
    result.percentage = calculate_percentage(battery_voltage);
    result.calibrated = calibrated;
    result.valid = true;

    s_latest = result;
    *reading = result;

    if (calibrated) {
        ESP_LOGI(
            TAG,
            "Battery：raw=%d rawApprox=%dmV/%.3fV calibrated=%dmV/%.3fV selected=%.3fV percentage=%u%%",
            result.raw_average,
            result.raw_approx_millivolts,
            static_cast<double>(result.raw_approx_battery_voltage),
            result.calibrated_millivolts,
            static_cast<double>(result.calibrated_battery_voltage),
            static_cast<double>(result.battery_voltage),
            result.percentage);
    } else {
        ESP_LOGI(
            TAG,
            "Battery：raw=%d rawApprox=%dmV/%.3fV calibrated=不可用 selected=%.3fV percentage=%u%%",
            result.raw_average,
            result.raw_approx_millivolts,
            static_cast<double>(result.raw_approx_battery_voltage),
            static_cast<double>(result.battery_voltage),
            result.percentage);
    }

    return ESP_OK;
}

BatteryReading battery_service_latest()
{
    return s_latest;
}
