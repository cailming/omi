#include "ota_service.h"
#include "omiglass_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

static const char *TAG = "ota";

constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
constexpr EventBits_t WIFI_FAILED_BIT = BIT1;
constexpr TickType_t WIFI_WAIT_SLICE_TICKS = pdMS_TO_TICKS(250);

struct OtaTaskContext {
    std::array<char, OMI_WIFI_MAX_SSID_LEN + 1> ssid{};
    std::array<char, OMI_WIFI_MAX_PASSWORD_LEN + 1> password{};
    std::array<char, OMI_OTA_MAX_URL_LEN + 1> url{};
};

std::atomic_bool s_busy{false};
std::atomic_bool s_cancel_requested{false};
std::atomic_bool s_wifi_connect_phase{false};
std::atomic<uint8_t> s_status{OMI_OTA_STATUS_IDLE};
std::atomic<uint8_t> s_progress{0};

OtaStatusHandler s_status_handler = nullptr;
SemaphoreHandle_t s_config_mutex = nullptr;
EventGroupHandle_t s_wifi_event_group = nullptr;
esp_netif_t *s_wifi_sta_netif = nullptr;

std::array<char, OMI_WIFI_MAX_SSID_LEN + 1> s_wifi_ssid{};
std::array<char, OMI_WIFI_MAX_PASSWORD_LEN + 1> s_wifi_password{};
std::array<char, OMI_OTA_MAX_URL_LEN + 1> s_firmware_url{};
bool s_wifi_credentials_set = false;
bool s_firmware_url_set = false;

OtaTaskContext s_task_context{};
TaskHandle_t s_ota_task_handle = nullptr;
bool s_service_initialized = false;
bool s_wifi_driver_initialized = false;
bool s_wifi_started = false;

void publish_status(uint8_t status, uint8_t progress = 0)
{
    s_status.store(status);
    s_progress.store(progress);

    ESP_LOGI(TAG, "OTA 状态=0x%02X，进度=%u%%", status, progress);

    if (s_status_handler != nullptr) {
        s_status_handler(status, progress);
    }
}

bool is_https_url(const char *url)
{
    return url != nullptr &&
           std::strncmp(url, OMI_OTA_HTTPS_PREFIX, std::strlen(OMI_OTA_HTTPS_PREFIX)) == 0;
}

void wifi_event_handler(void *, esp_event_base_t event_base, int32_t event_id, void *)
{
    if (s_wifi_event_group == nullptr) {
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect() 失败: %s", esp_err_to_name(err));
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAILED_BIT);
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Arduino WiFi.begin() 的旧实现会在整个 15 秒窗口内等待连接成功，
        // 不会因为第一次认证/关联失败就立即退出。Native 版本只在“连接阶段”
        // 自动重试，进入 HTTPS OTA 后若链路断开则交给 HTTP/TLS 错误路径处理。
        if (s_wifi_connect_phase.load() && !s_cancel_requested.load()) {
            const esp_err_t reconnect_err = esp_wifi_connect();
            if (reconnect_err != ESP_OK) {
                ESP_LOGE(TAG, "Wi-Fi 连接阶段重试失败: %s", esp_err_to_name(reconnect_err));
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAILED_BIT);
            } else {
                ESP_LOGD(TAG, "Wi-Fi 尚未连通，继续在超时窗口内重试");
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connect_phase.store(false);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t init_network_event_infrastructure()
{
    if (s_wifi_event_group == nullptr) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init() 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "创建默认 Event Loop 失败: %s", esp_err_to_name(err));
        return err;
    }

    if (s_wifi_sta_netif == nullptr) {
        s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_wifi_sta_netif == nullptr) {
            ESP_LOGE(TAG, "创建默认 Wi-Fi STA netif 失败");
            return ESP_FAIL;
        }

        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "注册 Wi-Fi Event Handler 失败: %s", esp_err_to_name(err));
            return err;
        }

        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "注册 IP Event Handler 失败: %s", esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t ensure_wifi_driver_initialized()
{
    if (s_wifi_driver_initialized) {
        return ESP_OK;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init() 失败: %s", esp_err_to_name(err));
        return err;
    }

    // 与原固件“临时凭据”语义一致：SSID / Password 只存在 RAM，不写 Wi-Fi NVS。
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置 Wi-Fi RAM Storage 失败: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        return err;
    }

    s_wifi_driver_initialized = true;
    return ESP_OK;
}

void stop_wifi()
{
    // 先退出连接阶段，避免主动 disconnect/stop 触发的 STA_DISCONNECTED
    // 被事件回调误判成“需要继续重连”。
    s_wifi_connect_phase.store(false);

    if (!s_wifi_driver_initialized) {
        return;
    }

    if (s_wifi_started) {
        esp_wifi_disconnect();
        const esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK) {
            ESP_LOGW(TAG, "停止 Wi-Fi 返回: %s", esp_err_to_name(stop_err));
        }
        s_wifi_started = false;
    }
}

void deinit_wifi_driver()
{
    stop_wifi();

    if (s_wifi_driver_initialized) {
        const esp_err_t err = esp_wifi_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_deinit() 返回: %s", esp_err_to_name(err));
        }
        s_wifi_driver_initialized = false;
    }
}

esp_err_t connect_wifi(const OtaTaskContext &context)
{
    publish_status(OMI_OTA_STATUS_WIFI_CONNECTING, 0);

    esp_err_t err = ensure_wifi_driver_initialized();
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t wifi_config{};
    const size_t ssid_len = std::strlen(context.ssid.data());
    const size_t password_len = std::strlen(context.password.data());

    std::memcpy(wifi_config.sta.ssid, context.ssid.data(), std::min(ssid_len, sizeof(wifi_config.sta.ssid)));
    std::memcpy(
        wifi_config.sta.password,
        context.password.data(),
        std::min(password_len, sizeof(wifi_config.sta.password)));

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "连接 OTA Wi-Fi，SSID=%s", context.ssid.data());

    // 从 esp_wifi_start() 到 GOT_IP / cancel / timeout 都属于连接阶段。
    // 期间 STA_DISCONNECTED 事件会继续发起连接，以对齐旧 WiFi.begin() 的等待语义。
    s_wifi_connect_phase.store(true);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        s_wifi_connect_phase.store(false);
        return err;
    }
    s_wifi_started = true;

    const TickType_t start_tick = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(OMI_WIFI_CONNECT_TIMEOUT_MS);

    while (true) {
        if (s_cancel_requested.load()) {
            s_wifi_connect_phase.store(false);
            ESP_LOGW(TAG, "Wi-Fi 连接阶段收到 OTA 取消请求");
            return ESP_ERR_INVALID_STATE;
        }

        const EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
            pdFALSE,
            pdFALSE,
            WIFI_WAIT_SLICE_TICKS);

        if ((bits & WIFI_CONNECTED_BIT) != 0) {
            s_wifi_connect_phase.store(false);
            publish_status(OMI_OTA_STATUS_WIFI_CONNECTED, 0);
            return ESP_OK;
        }

        if ((bits & WIFI_FAILED_BIT) != 0) {
            s_wifi_connect_phase.store(false);
            ESP_LOGW(TAG, "Wi-Fi 连接调用失败");
            return ESP_FAIL;
        }

        if (xTaskGetTickCount() - start_tick >= timeout_ticks) {
            s_wifi_connect_phase.store(false);
            ESP_LOGW(TAG, "Wi-Fi 连接超时（%u ms）", OMI_WIFI_CONNECT_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
    }
}

esp_err_t perform_https_ota(const OtaTaskContext &context)
{
    if (!is_https_url(context.url.data())) {
        ESP_LOGE(TAG, "拒绝非 HTTPS OTA URL");
        publish_status(OMI_OTA_STATUS_ERROR, 0);
        return ESP_ERR_INVALID_ARG;
    }

    publish_status(OMI_OTA_STATUS_DOWNLOADING, 0);

    esp_http_client_config_t http_config{};
    http_config.url = context.url.data();
    http_config.timeout_ms = static_cast<int>(OMI_OTA_HTTP_TIMEOUT_MS);
    // 保持 Arduino OTA 请求的 User-Agent，避免已有 CDN/WAF/下载服务按 UA 做兼容规则时行为变化。
    http_config.user_agent = "ESP32-OTA/1.0";
    http_config.keep_alive_enable = true;
    http_config.max_redirection_count = 5;

    // 使用 ESP-IDF 内置 Mozilla Root CA Bundle 验证服务器证书与主机名。
    // 不设置 skip_cert_common_name_check，也不启用任何 insecure 选项。
    http_config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_https_ota_config_t ota_config{};
    ota_config.http_config = &http_config;

    esp_https_ota_handle_t ota_handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK || ota_handle == nullptr) {
        ESP_LOGE(TAG, "esp_https_ota_begin() 失败: %s", esp_err_to_name(err));
        publish_status(OMI_OTA_STATUS_DOWNLOAD_FAILED, 0);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    const int http_status = esp_https_ota_get_status_code(ota_handle);
    if (http_status >= 400) {
        ESP_LOGE(TAG, "OTA HTTP 状态码异常: %d", http_status);
        esp_https_ota_abort(ota_handle);
        publish_status(OMI_OTA_STATUS_DOWNLOAD_FAILED, 0);
        return ESP_FAIL;
    }

    esp_app_desc_t new_app_info{};
    if (esp_https_ota_get_img_desc(ota_handle, &new_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "待安装固件版本: %s，Project=%s", new_app_info.version, new_app_info.project_name);
    }

    const int image_size = esp_https_ota_get_image_size(ota_handle);
    if (image_size > 0) {
        ESP_LOGI(TAG, "OTA 镜像大小: %d bytes", image_size);
    } else {
        ESP_LOGW(TAG, "服务器未提供固定镜像大小，将只在完成时上报 100%%");
    }

    publish_status(OMI_OTA_STATUS_INSTALLING, 0);
    uint8_t last_reported_progress = 0;

    while (true) {
        if (s_cancel_requested.load()) {
            ESP_LOGW(TAG, "HTTPS OTA 已取消，终止当前写入");
            esp_https_ota_abort(ota_handle);
            publish_status(OMI_OTA_STATUS_IDLE, 0);
            return ESP_ERR_INVALID_STATE;
        }

        err = esp_https_ota_perform(ota_handle);

        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            if (image_size > 0) {
                const int bytes_read = esp_https_ota_get_image_len_read(ota_handle);
                if (bytes_read >= 0) {
                    const int raw_progress = std::clamp((bytes_read * 100) / image_size, 0, 99);
                    const uint8_t stepped_progress = static_cast<uint8_t>(
                        (raw_progress / OMI_OTA_PROGRESS_STEP_PERCENT) * OMI_OTA_PROGRESS_STEP_PERCENT);

                    if (stepped_progress >= OMI_OTA_PROGRESS_STEP_PERCENT &&
                        stepped_progress > last_reported_progress) {
                        last_reported_progress = stepped_progress;
                        publish_status(OMI_OTA_STATUS_INSTALLING, stepped_progress);
                    }
                }
            }
            continue;
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_https_ota_perform() 失败: %s", esp_err_to_name(err));
            esp_https_ota_abort(ota_handle);
            publish_status(OMI_OTA_STATUS_INSTALL_FAILED, last_reported_progress);
            return err;
        }

        break;
    }

    if (s_cancel_requested.load()) {
        esp_https_ota_abort(ota_handle);
        publish_status(OMI_OTA_STATUS_IDLE, 0);
        return ESP_ERR_INVALID_STATE;
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "OTA 镜像未完整接收");
        esp_https_ota_abort(ota_handle);
        publish_status(OMI_OTA_STATUS_DOWNLOAD_FAILED, last_reported_progress);
        return ESP_FAIL;
    }

    // finish() 会校验镜像并切换下一次启动使用的 OTA 分区。
    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish() 失败: %s", esp_err_to_name(err));
        publish_status(OMI_OTA_STATUS_INSTALL_FAILED, last_reported_progress);
        return err;
    }

    publish_status(OMI_OTA_STATUS_INSTALL_COMPLETE, 100);
    return ESP_OK;
}

void finish_failed_or_cancelled_task()
{
    stop_wifi();
    s_ota_task_handle = nullptr;
    s_busy.store(false);
    vTaskDelete(nullptr);
}

void ota_task(void *)
{
    ESP_LOGI(TAG, "OTA Task 启动");

    esp_err_t err = connect_wifi(s_task_context);
    if (err != ESP_OK) {
        if (s_cancel_requested.load()) {
            publish_status(OMI_OTA_STATUS_IDLE, 0);
        } else {
            publish_status(OMI_OTA_STATUS_WIFI_FAILED, 0);
        }
        finish_failed_or_cancelled_task();
    }

    if (s_cancel_requested.load()) {
        publish_status(OMI_OTA_STATUS_IDLE, 0);
        finish_failed_or_cancelled_task();
    }

    err = perform_https_ota(s_task_context);
    if (err != ESP_OK) {
        finish_failed_or_cancelled_task();
    }

    // 安装完成后保持 busy=true，避免重启提示期间电源管理进入睡眠。
    publish_status(OMI_OTA_STATUS_REBOOTING, 100);
    vTaskDelay(pdMS_TO_TICKS(OMI_OTA_REBOOT_NOTICE_MS));

    stop_wifi();
    ESP_LOGI(TAG, "OTA 成功，立即重启进入新固件");
    esp_restart();

    // esp_restart() 正常不会返回。
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}

bool copy_start_context()
{
    if (s_config_mutex == nullptr ||
        xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    const bool valid = s_wifi_credentials_set && s_firmware_url_set;
    if (valid) {
        s_task_context.ssid = s_wifi_ssid;
        s_task_context.password = s_wifi_password;
        s_task_context.url = s_firmware_url;
    }

    xSemaphoreGive(s_config_mutex);
    return valid;
}

void handle_set_wifi(const uint8_t *data, size_t length)
{
    // 格式保持原协议：[cmd, ssid_len, ssid..., pass_len, pass...]
    if (s_busy.load() || length < 3) {
        publish_status(OMI_OTA_STATUS_ERROR, 0);
        return;
    }

    const size_t ssid_len = data[1];
    const size_t pass_len_index = 2 + ssid_len;

    if (ssid_len > OMI_WIFI_MAX_SSID_LEN || pass_len_index >= length) {
        publish_status(OMI_OTA_STATUS_ERROR, 0);
        return;
    }

    const size_t password_len = data[pass_len_index];
    if (password_len > OMI_WIFI_MAX_PASSWORD_LEN ||
        length < 3 + ssid_len + password_len) {
        publish_status(OMI_OTA_STATUS_ERROR, 0);
        return;
    }

    std::array<char, OMI_WIFI_MAX_SSID_LEN + 1> ssid{};
    std::array<char, OMI_WIFI_MAX_PASSWORD_LEN + 1> password{};
    std::memcpy(ssid.data(), &data[2], ssid_len);
    std::memcpy(password.data(), &data[3 + ssid_len], password_len);

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        publish_status(OMI_OTA_STATUS_ERROR, 0);
        return;
    }

    s_wifi_ssid = ssid;
    s_wifi_password = password;
    s_wifi_credentials_set = true;
    xSemaphoreGive(s_config_mutex);

    ESP_LOGI(TAG, "OTA Wi-Fi 凭据已更新，SSID=%s，密码不写入日志/NVS", ssid.data());
    publish_status(OMI_OTA_STATUS_IDLE, 0);
}

void handle_set_url(const uint8_t *data, size_t length)
{
    // 格式保持原协议：[cmd, url_len_hi, url_len_lo, url...]
    if (s_busy.load() || length < 4) {
        publish_status(OMI_OTA_STATUS_ERROR, 0);
        return;
    }

    const size_t url_len = (static_cast<size_t>(data[1]) << 8U) | data[2];
    if (url_len == 0 || url_len > OMI_OTA_MAX_URL_LEN || length < 3 + url_len) {
        publish_status(OMI_OTA_STATUS_ERROR, 0);
        return;
    }

    std::array<char, OMI_OTA_MAX_URL_LEN + 1> url{};
    std::memcpy(url.data(), &data[3], url_len);

    // Phase 10 不再允许明文 HTTP 或 setInsecure()。
    if (!is_https_url(url.data())) {
        ESP_LOGW(TAG, "拒绝 OTA URL：仅允许 https:// 地址");
        publish_status(OMI_OTA_STATUS_ERROR, 0);
        return;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        publish_status(OMI_OTA_STATUS_ERROR, 0);
        return;
    }

    s_firmware_url = url;
    s_firmware_url_set = true;
    xSemaphoreGive(s_config_mutex);

    // OTA 地址可能是带签名参数的临时 URL，完整内容不写日志，避免泄露 query token。
    ESP_LOGI(TAG, "OTA HTTPS URL 已更新，长度=%u；完整 URL 不写入日志", static_cast<unsigned>(url_len));
    publish_status(OMI_OTA_STATUS_IDLE, 0);
}

void handle_start_ota()
{
    if (s_busy.load()) {
        ESP_LOGW(TAG, "OTA 已在执行，忽略重复 START 命令");
        publish_status(s_status.load(), s_progress.load());
        return;
    }

    if (!copy_start_context()) {
        ESP_LOGW(TAG, "OTA START 前必须同时设置 Wi-Fi 凭据和 HTTPS URL");
        publish_status(OMI_OTA_STATUS_ERROR, 0);
        return;
    }

    s_cancel_requested.store(false);
    s_busy.store(true);

    const BaseType_t created = xTaskCreate(
        ota_task,
        "omi_https_ota",
        OMI_OTA_TASK_STACK_SIZE,
        nullptr,
        OMI_OTA_TASK_PRIORITY,
        &s_ota_task_handle);

    if (created != pdPASS) {
        s_ota_task_handle = nullptr;
        s_busy.store(false);
        ESP_LOGE(TAG, "创建 OTA Task 失败");
        publish_status(OMI_OTA_STATUS_ERROR, 0);
    }
}

} // namespace

esp_err_t ota_service_init()
{
    if (s_service_initialized) {
        return ESP_OK;
    }

    s_config_mutex = xSemaphoreCreateMutex();
    if (s_config_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = init_network_event_infrastructure();
    if (err != ESP_OK) {
        vSemaphoreDelete(s_config_mutex);
        s_config_mutex = nullptr;
        return err;
    }

    s_wifi_credentials_set = false;
    s_firmware_url_set = false;
    s_wifi_ssid.fill('\0');
    s_wifi_password.fill('\0');
    s_firmware_url.fill('\0');
    s_cancel_requested.store(false);
    s_busy.store(false);
    s_wifi_connect_phase.store(false);
    s_status.store(OMI_OTA_STATUS_IDLE);
    s_progress.store(0);
    s_service_initialized = true;

    ESP_LOGI(TAG, "Phase 10 OTA Service 初始化完成；Wi-Fi 保持关闭，等待 BLE OTA 命令");
    return ESP_OK;
}

void ota_service_deinit()
{
    if (!s_service_initialized) {
        return;
    }

    if (s_busy.load()) {
        ESP_LOGW(TAG, "OTA 正在执行，先请求取消；本次不强制释放正在使用的 OTA 资源");
        ota_service_cancel();
        return;
    }

    deinit_wifi_driver();

    if (s_config_mutex != nullptr) {
        vSemaphoreDelete(s_config_mutex);
        s_config_mutex = nullptr;
    }

    // 默认 Event Loop / netif 可能在后续阶段被其他网络能力复用，
    // 因此这里不主动销毁全局网络基础设施。
    s_service_initialized = false;
}

void ota_service_set_status_handler(OtaStatusHandler handler)
{
    s_status_handler = handler;
}

void ota_service_handle_command(const uint8_t *data, size_t length)
{
    if (!s_service_initialized || data == nullptr || length == 0) {
        return;
    }

    const uint8_t command = data[0];
    ESP_LOGI(TAG, "收到 OTA Control 命令 0x%02X，长度=%u", command, static_cast<unsigned>(length));

    switch (command) {
    case OMI_OTA_CMD_SET_WIFI:
        handle_set_wifi(data, length);
        break;

    case OMI_OTA_CMD_SET_URL:
        handle_set_url(data, length);
        break;

    case OMI_OTA_CMD_START_OTA:
        handle_start_ota();
        break;

    case OMI_OTA_CMD_CANCEL_OTA:
        ota_service_cancel();
        break;

    case OMI_OTA_CMD_GET_STATUS:
        publish_status(s_status.load(), s_progress.load());
        break;

    default:
        ESP_LOGW(TAG, "未知 OTA 命令 0x%02X", command);
        publish_status(OMI_OTA_STATUS_ERROR, 0);
        break;
    }
}

void ota_service_cancel()
{
    if (!s_busy.load()) {
        return;
    }

    ESP_LOGW(TAG, "收到 OTA 取消请求");
    s_cancel_requested.store(true);
}

bool ota_service_is_busy()
{
    return s_busy.load();
}

uint8_t ota_service_status()
{
    return s_status.load();
}

uint8_t ota_service_progress()
{
    return s_progress.load();
}
