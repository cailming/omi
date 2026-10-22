#include "board_service.h"

#include <cinttypes>
#include <cstdio>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "omiglass_config.h"

namespace {

static const char *TAG = "board";

// 中断函数只做最小工作：记录 GPIO 状态发生过变化。
// 去抖、长按判断等逻辑全部放到普通任务上下文中执行。
volatile bool s_button_irq_pending = false;

bool s_button_down = false;
bool s_long_press_triggered = false;
bool s_connected = false;
uint64_t s_button_press_time_ms = 0;
uint64_t s_last_debounce_time_ms = 0;
uint64_t s_boot_start_time_ms = 0;
BoardButtonEvent s_pending_button_event = BoardButtonEvent::NONE;

// 原 Arduino 固件使用 char serialNumber[17]，这里保持相同容量。
char s_device_serial[17] = {0};

void IRAM_ATTR button_isr(void *)
{
    s_button_irq_pending = true;
}

esp_err_t init_device_serial()
{
    // Arduino ESP.getEfuseMac() 的实现是把默认 eFuse MAC 的 6 个字节
    // 直接写入 uint64_t，然后原代码再按数值格式输出。
    // ESP32-S3 为小端架构，因此这里采用相同方式，尽量保持序列号完全兼容。
    uint64_t chip_id = 0;
    esp_err_t err = esp_efuse_mac_get_default(reinterpret_cast<uint8_t *>(&chip_id));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取 eFuse MAC 失败: %s", esp_err_to_name(err));
        return err;
    }

    // 使用 PRIX32 而不是假设 uint32_t 一定对应 unsigned int。
    // ESP-IDF v6.1 的当前工具链会把错误的 %X 类型匹配提升为 -Werror=format。
    std::snprintf(
        s_device_serial,
        sizeof(s_device_serial),
        "%04" PRIX32 "%08" PRIX32,
        static_cast<uint32_t>((chip_id >> 32) & 0xFFFFULL),
        static_cast<uint32_t>(chip_id & 0xFFFFFFFFULL));

    ESP_LOGI(TAG, "设备序列号: %s", s_device_serial);
    return ESP_OK;
}

esp_err_t init_led_gpio()
{
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << OMI_STATUS_LED_GPIO;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }

    // 与 Arduino 固件一致：启动时先关闭 LED，之后进入启动闪烁序列。
    return gpio_set_level(OMI_STATUS_LED_GPIO, OMI_LED_OFF_LEVEL);
}

esp_err_t init_button_gpio()
{
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << OMI_POWER_BUTTON_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_ANYEDGE;

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }

    // 若 ISR 服务已经由后续其他模块安装，ESP-IDF 会返回 ESP_ERR_INVALID_STATE。
    // 这种情况可以继续复用已有 ISR 服务。
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    return gpio_isr_handler_add(OMI_POWER_BUTTON_GPIO, button_isr, nullptr);
}

void set_led(bool on)
{
    gpio_set_level(OMI_STATUS_LED_GPIO, on ? OMI_LED_ON_LEVEL : OMI_LED_OFF_LEVEL);
}

void process_button(uint64_t now_ms)
{
    // 按键按住期间即使没有新的边沿中断，也必须持续检查是否达到长按阈值。
    if (!s_button_irq_pending && !s_button_down) {
        return;
    }

    const bool pressed = gpio_get_level(OMI_POWER_BUTTON_GPIO) == OMI_BUTTON_PRESSED_LEVEL;

    if (pressed && !s_button_down) {
        if (now_ms - s_last_debounce_time_ms < OMI_BUTTON_DEBOUNCE_MS) {
            return;
        }

        s_button_down = true;
        s_long_press_triggered = false;
        s_button_press_time_ms = now_ms;
        s_last_debounce_time_ms = now_ms;
        ESP_LOGD(TAG, "按键按下");
    } else if (pressed && s_button_down && !s_long_press_triggered) {
        if (now_ms - s_button_press_time_ms >= OMI_POWER_OFF_PRESS_MS) {
            s_long_press_triggered = true;
            s_pending_button_event = BoardButtonEvent::LONG_PRESS;
            ESP_LOGI(TAG, "检测到长按事件，交给 Power Service 处理关机 / Deep Sleep");
        }
    } else if (!pressed && s_button_down) {
        if (now_ms - s_last_debounce_time_ms < OMI_BUTTON_DEBOUNCE_MS) {
            return;
        }

        const uint64_t press_duration_ms = now_ms - s_button_press_time_ms;
        s_button_down = false;
        s_last_debounce_time_ms = now_ms;

        if (!s_long_press_triggered && press_duration_ms >= OMI_BUTTON_DEBOUNCE_MS) {
            s_pending_button_event = BoardButtonEvent::SHORT_PRESS;
            ESP_LOGI(TAG, "检测到短按事件，持续 %llu ms", static_cast<unsigned long long>(press_duration_ms));
        }

        s_long_press_triggered = false;
    }

    s_button_irq_pending = false;
}

void process_led(uint64_t now_ms)
{
    const uint64_t boot_elapsed_ms = now_ms - s_boot_start_time_ms;

    if (boot_elapsed_ms < OMI_BOOT_SEQUENCE_MS) {
        // 与 Arduino 版本一致：启动阶段约 1.5 秒，以 150ms 为半周期快速闪烁。
        const bool led_on = ((boot_elapsed_ms / OMI_BOOT_BLINK_HALF_PERIOD_MS) % 2U) != 0U;
        set_led(led_on);
        return;
    }

    if (s_connected) {
        // BLE 已连接时保持常亮。BLE 模块通过 set_connected() 更新状态。
        set_led(true);
        return;
    }

    // BLE 未连接状态下每秒翻转一次 LED。
    const bool led_on = ((now_ms / OMI_DISCONNECTED_BLINK_PERIOD_MS) % 2U) == 0U;
    set_led(led_on);
}

} // namespace

esp_err_t board_service_init()
{
    s_boot_start_time_ms = board_millis();

    esp_err_t err = init_device_serial();
    if (err != ESP_OK) {
        return err;
    }

    err = init_led_gpio();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "初始化状态 LED 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = init_button_gpio();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "初始化电源按键失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "板级基础服务初始化完成：按键 GPIO%d，LED GPIO%d", OMI_POWER_BUTTON_GPIO, OMI_STATUS_LED_GPIO);
    return ESP_OK;
}

void board_service_process()
{
    const uint64_t now_ms = board_millis();
    process_button(now_ms);
    process_led(now_ms);
}

BoardButtonEvent board_service_take_button_event()
{
    const BoardButtonEvent event = s_pending_button_event;
    s_pending_button_event = BoardButtonEvent::NONE;
    return event;
}

void board_service_set_connected(bool connected)
{
    s_connected = connected;
}

uint64_t board_millis()
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

void board_delay_ms(uint32_t delay_ms)
{
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

const char *board_device_serial()
{
    return s_device_serial;
}
