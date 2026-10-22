#include "power_service.h"

#include <cstdint>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "omiglass_config.h"

namespace {

static const char *TAG = "power";

// =============================================================================
// Phase 9 电源策略参数
// 保留原 Arduino 固件的 80MHz 正常频率、40MHz 省电频率和时间阈值。
// ESP32-S3 的 ESP_PM_CPU_FREQ_MAX 合法上限档位为 80/160/240MHz，
// 因此迁移版本使用 80MHz 作为业务最高频率，不使用原 config.h 中未真正启用的 100MHz 值。
// =============================================================================
constexpr int POWER_MAX_CPU_FREQ_MHZ = 80;
constexpr int POWER_MIN_CPU_FREQ_MHZ = 40;

constexpr uint64_t POWER_LIGHT_SLEEP_IDLE_MS = 5000;
constexpr uint64_t POWER_SAVE_IDLE_MS = 45000;
constexpr uint64_t POWER_DEEP_SLEEP_IDLE_MS = 300000;

constexpr uint64_t POWER_OFF_SEQUENCE_MS = 800;
constexpr uint64_t POWER_OFF_BLINK_HALF_PERIOD_MS = 200;
constexpr uint64_t LOW_BATTERY_BLINK_HALF_PERIOD_MS = 1000;
constexpr uint32_t BLE_STOP_SETTLE_MS = 20;

esp_pm_lock_handle_t s_cpu_max_lock = nullptr;
esp_pm_lock_handle_t s_no_light_sleep_lock = nullptr;

bool s_cpu_max_locked = false;
bool s_no_light_sleep_locked = false;
bool s_initialized = false;

PowerState s_state = PowerState::ACTIVE;
PowerShutdownReason s_shutdown_reason = PowerShutdownReason::NONE;
PowerPrepareDeepSleepHandler s_prepare_handler = nullptr;

uint64_t s_last_activity_ms = 0;
uint64_t s_power_off_start_ms = 0;

bool s_last_connected = false;
bool s_last_audio_streaming = false;
bool s_last_photo_uploading = false;

void set_status_led(bool on)
{
    // GPIO21 在 OmiGlass 上使用反相逻辑：LOW=亮，HIGH=灭。
    gpio_set_level(OMI_STATUS_LED_GPIO, on ? OMI_LED_ON_LEVEL : OMI_LED_OFF_LEVEL);
}

void mark_activity(uint64_t now_ms)
{
    s_last_activity_ms = now_ms;
}

void acquire_cpu_max_lock()
{
    if (s_cpu_max_lock == nullptr || s_cpu_max_locked) {
        return;
    }

    const esp_err_t err = esp_pm_lock_acquire(s_cpu_max_lock);
    if (err == ESP_OK) {
        s_cpu_max_locked = true;
    } else {
        ESP_LOGW(TAG, "获取 CPU Max PM Lock 失败: %s", esp_err_to_name(err));
    }
}

void release_cpu_max_lock()
{
    if (s_cpu_max_lock == nullptr || !s_cpu_max_locked) {
        return;
    }

    const esp_err_t err = esp_pm_lock_release(s_cpu_max_lock);
    if (err == ESP_OK) {
        s_cpu_max_locked = false;
    } else {
        ESP_LOGW(TAG, "释放 CPU Max PM Lock 失败: %s", esp_err_to_name(err));
    }
}

void acquire_no_light_sleep_lock()
{
    if (s_no_light_sleep_lock == nullptr || s_no_light_sleep_locked) {
        return;
    }

    const esp_err_t err = esp_pm_lock_acquire(s_no_light_sleep_lock);
    if (err == ESP_OK) {
        s_no_light_sleep_locked = true;
    } else {
        ESP_LOGW(TAG, "获取 No-Light-Sleep PM Lock 失败: %s", esp_err_to_name(err));
    }
}

void release_no_light_sleep_lock()
{
    if (s_no_light_sleep_lock == nullptr || !s_no_light_sleep_locked) {
        return;
    }

    const esp_err_t err = esp_pm_lock_release(s_no_light_sleep_lock);
    if (err == ESP_OK) {
        s_no_light_sleep_locked = false;
    } else {
        ESP_LOGW(TAG, "释放 No-Light-Sleep PM Lock 失败: %s", esp_err_to_name(err));
    }
}

const char *shutdown_reason_name(PowerShutdownReason reason)
{
    switch (reason) {
    case PowerShutdownReason::USER_LONG_PRESS:
        return "用户长按";
    case PowerShutdownReason::CRITICAL_BATTERY:
        return "临界低电";
    case PowerShutdownReason::IDLE_TIMEOUT:
        return "长时间离线空闲";
    case PowerShutdownReason::NONE:
    default:
        return "无";
    }
}

void set_state(PowerState new_state)
{
    if (new_state == s_state) {
        return;
    }

    s_state = new_state;
    ESP_LOGI(TAG, "电源状态切换 -> %s", power_service_state_name());
}

void request_shutdown(PowerShutdownReason reason, uint64_t now_ms)
{
    if (s_state == PowerState::POWERING_OFF) {
        return;
    }

    s_shutdown_reason = reason;
    s_power_off_start_ms = now_ms;
    set_state(PowerState::POWERING_OFF);

    // 关机动画和资源释放期间保持 CPU 与系统唤醒，防止进入自动 Light Sleep。
    acquire_cpu_max_lock();
    acquire_no_light_sleep_lock();

    ESP_LOGW(TAG, "开始关机流程，原因=%s", shutdown_reason_name(reason));
}

void stop_nimble_before_deep_sleep()
{
    // ESP-IDF 文档要求进入 Deep Sleep 前停止 Bluetooth。
    // nimble_port_stop() 会让 nimble_port_run() 返回，Host Task 随后执行自身清理。
    const int stop_rc = nimble_port_stop();
    if (stop_rc != 0) {
        ESP_LOGW(TAG, "停止 NimBLE Host 返回 rc=%d；继续进入 Deep Sleep", stop_rc);
        return;
    }

    // 给 NimBLE Host Task 一小段时间退出并执行 nimble_port_freertos_deinit()。
    vTaskDelay(pdMS_TO_TICKS(BLE_STOP_SETTLE_MS));

    const esp_err_t deinit_err = nimble_port_deinit();
    if (deinit_err != ESP_OK) {
        ESP_LOGW(TAG, "NimBLE deinit 失败: %s；Deep Sleep 仍继续", esp_err_to_name(deinit_err));
    }
}

[[noreturn]] void enter_deep_sleep()
{
    ESP_LOGI(TAG, "准备进入 Deep Sleep，GPIO%d 低电平唤醒", OMI_POWER_BUTTON_GPIO);

    set_status_led(false);

    if (s_prepare_handler != nullptr) {
        s_prepare_handler();
    }

    stop_nimble_before_deep_sleep();

    // 清除先前可能配置过的唤醒源，再只保留电源按键。
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    // GPIO1 在 ESP32-S3 上属于 RTC GPIO。
    // Deep Sleep 时保持输入上拉，按键按下（低电平）通过新的 ext1 wakeup API 唤醒。
    esp_err_t err = rtc_gpio_init(OMI_POWER_BUTTON_GPIO);
    if (err == ESP_OK) {
        err = rtc_gpio_set_direction(OMI_POWER_BUTTON_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    }
    if (err == ESP_OK) {
        err = rtc_gpio_pullup_en(OMI_POWER_BUTTON_GPIO);
    }
    if (err == ESP_OK) {
        err = rtc_gpio_pulldown_dis(OMI_POWER_BUTTON_GPIO);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "配置 Deep Sleep RTC GPIO 失败: %s", esp_err_to_name(err));
    }

    // 保持 RTC 外设域供电，确保内部上拉在 Deep Sleep 中可靠有效。
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    err = esp_sleep_enable_ext1_wakeup_io(
        1ULL << static_cast<uint64_t>(OMI_POWER_BUTTON_GPIO),
        ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 GPIO1 Deep Sleep 唤醒失败: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "进入 Deep Sleep");
    esp_deep_sleep_start();

    // esp_deep_sleep_start() 正常不会返回。
    while (true) {
    }
}

void process_powering_off(uint64_t now_ms)
{
    const uint64_t elapsed_ms = now_ms - s_power_off_start_ms;

    // 与原 Arduino 固件一致：关机阶段 200ms 半周期快速闪烁，约 800ms。
    const bool led_on = ((elapsed_ms / POWER_OFF_BLINK_HALF_PERIOD_MS) % 2U) == 0U;
    set_status_led(led_on);

    if (elapsed_ms < POWER_OFF_SEQUENCE_MS) {
        return;
    }

    // 长按事件发生时用户的手指往往还压在 GPIO1 上。
    // 如果此时直接进入“低电平唤醒”的 Deep Sleep，会立刻重新开机。
    // 因此用户长按关机时等待按键释放；低电和空闲超时不需要等待。
    if (s_shutdown_reason == PowerShutdownReason::USER_LONG_PRESS &&
        gpio_get_level(OMI_POWER_BUTTON_GPIO) == OMI_BUTTON_PRESSED_LEVEL) {
        set_status_led(false);
        return;
    }

    enter_deep_sleep();
}

void apply_runtime_led_override(uint64_t now_ms)
{
    if (s_state == PowerState::POWERING_OFF) {
        return;
    }

    if (s_state == PowerState::LOW_BATTERY) {
        // 低电状态采用 1 秒半周期慢闪；每轮在 board_service 更新后覆盖 LED。
        const bool led_on = ((now_ms / LOW_BATTERY_BLINK_HALF_PERIOD_MS) % 2U) == 0U;
        set_status_led(led_on);
    }
}

void update_runtime_locks(uint64_t now_ms, const PowerServiceInputs &inputs)
{
    const uint64_t idle_ms = now_ms - s_last_activity_ms;
    const bool high_bandwidth_active = inputs.audio_streaming || inputs.photo_uploading;

    // CPU 频率：
    // 1. 音频 / 图片实时业务始终保持 80MHz；
    // 2. 普通 BLE 连接保持原固件 80MHz 行为；
    // 3. 离线空闲超过 45 秒后允许降至 40MHz；
    // 4. 低电模式除实时业务外优先允许降频。
    bool need_max_cpu = high_bandwidth_active || inputs.ble_connected;
    if (!inputs.ble_connected && idle_ms >= POWER_SAVE_IDLE_MS) {
        need_max_cpu = false;
    }
    if (s_state == PowerState::LOW_BATTERY && !high_bandwidth_active) {
        need_max_cpu = false;
    }

    if (need_max_cpu) {
        acquire_cpu_max_lock();
    } else {
        release_cpu_max_lock();
    }

    // Light Sleep：
    // 使用 ESP-IDF 自动 Light Sleep，而不是手工 esp_light_sleep_start()。
    // 音频、图片或最近 5 秒内有活动时禁止睡眠，其余时间释放锁让 Tickless Idle 决定睡多久。
    const bool block_light_sleep = high_bandwidth_active || idle_ms < POWER_LIGHT_SLEEP_IDLE_MS;
    if (block_light_sleep) {
        acquire_no_light_sleep_lock();
    } else {
        release_no_light_sleep_lock();
    }
}

void update_activity_edges(uint64_t now_ms, const PowerServiceInputs &inputs)
{
    if (inputs.short_press) {
        mark_activity(now_ms);
    }

    if (inputs.ble_connected != s_last_connected) {
        mark_activity(now_ms);
        s_last_connected = inputs.ble_connected;
    }

    if (inputs.audio_streaming && !s_last_audio_streaming) {
        mark_activity(now_ms);
    }
    s_last_audio_streaming = inputs.audio_streaming;

    if (inputs.photo_uploading && !s_last_photo_uploading) {
        mark_activity(now_ms);
    }
    s_last_photo_uploading = inputs.photo_uploading;
}

} // namespace

esp_err_t power_service_init()
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_pm_config_t pm_config{};
    pm_config.max_freq_mhz = POWER_MAX_CPU_FREQ_MHZ;
    pm_config.min_freq_mhz = POWER_MIN_CPU_FREQ_MHZ;
    pm_config.light_sleep_enable = true;

    esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_configure() 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "omi_cpu_max", &s_cpu_max_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建 CPU Max PM Lock 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "omi_awake", &s_no_light_sleep_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建 No-Light-Sleep PM Lock 失败: %s", esp_err_to_name(err));
        esp_pm_lock_delete(s_cpu_max_lock);
        s_cpu_max_lock = nullptr;
        return err;
    }

    s_last_activity_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
    s_power_off_start_ms = 0;
    s_state = PowerState::ACTIVE;
    s_shutdown_reason = PowerShutdownReason::NONE;
    s_last_connected = false;
    s_last_audio_streaming = false;
    s_last_photo_uploading = false;

    // 启动后的前几秒保持 80MHz 且不进入 Light Sleep，避免初始化阶段频繁切换电源状态。
    acquire_cpu_max_lock();
    acquire_no_light_sleep_lock();

    s_initialized = true;

    ESP_LOGI(
        TAG,
        "Phase 9 Power Management 初始化完成：CPU=%d~%dMHz，自动 Light Sleep=启用",
        POWER_MIN_CPU_FREQ_MHZ,
        POWER_MAX_CPU_FREQ_MHZ);
    return ESP_OK;
}

void power_service_deinit()
{
    if (!s_initialized) {
        return;
    }

    release_no_light_sleep_lock();
    release_cpu_max_lock();

    if (s_no_light_sleep_lock != nullptr) {
        esp_pm_lock_delete(s_no_light_sleep_lock);
        s_no_light_sleep_lock = nullptr;
    }
    if (s_cpu_max_lock != nullptr) {
        esp_pm_lock_delete(s_cpu_max_lock);
        s_cpu_max_lock = nullptr;
    }

    s_initialized = false;
}

void power_service_set_prepare_deep_sleep_handler(PowerPrepareDeepSleepHandler handler)
{
    s_prepare_handler = handler;
}

void power_service_process(uint64_t now_ms, const PowerServiceInputs &inputs)
{
    if (!s_initialized) {
        return;
    }

    update_activity_edges(now_ms, inputs);

    if (s_state == PowerState::POWERING_OFF) {
        process_powering_off(now_ms);
        return;
    }

    if (inputs.long_press) {
        request_shutdown(PowerShutdownReason::USER_LONG_PRESS, now_ms);
        process_powering_off(now_ms);
        return;
    }

    if (inputs.battery_valid && inputs.battery_voltage <= OMI_BATTERY_CRITICAL_VOLTAGE) {
        request_shutdown(PowerShutdownReason::CRITICAL_BATTERY, now_ms);
        process_powering_off(now_ms);
        return;
    }

    const uint64_t idle_ms = now_ms - s_last_activity_ms;
    if (!inputs.ble_connected && !inputs.audio_streaming && !inputs.photo_uploading &&
        idle_ms >= POWER_DEEP_SLEEP_IDLE_MS) {
        request_shutdown(PowerShutdownReason::IDLE_TIMEOUT, now_ms);
        process_powering_off(now_ms);
        return;
    }

    if (inputs.battery_valid && inputs.battery_voltage <= OMI_BATTERY_LOW_VOLTAGE) {
        set_state(PowerState::LOW_BATTERY);
    } else if (!inputs.ble_connected && !inputs.audio_streaming && !inputs.photo_uploading &&
               idle_ms >= POWER_SAVE_IDLE_MS) {
        set_state(PowerState::POWER_SAVE);
    } else {
        set_state(PowerState::ACTIVE);
    }

    update_runtime_locks(now_ms, inputs);
    apply_runtime_led_override(now_ms);
}

PowerState power_service_state()
{
    return s_state;
}

PowerShutdownReason power_service_shutdown_reason()
{
    return s_shutdown_reason;
}

const char *power_service_state_name()
{
    switch (s_state) {
    case PowerState::ACTIVE:
        return "ACTIVE";
    case PowerState::POWER_SAVE:
        return "POWER_SAVE";
    case PowerState::LOW_BATTERY:
        return "LOW_BATTERY";
    case PowerState::POWERING_OFF:
        return "POWERING_OFF";
    default:
        return "UNKNOWN";
    }
}
