#include "ble_service.h"
#include "omiglass_config.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

namespace {

static const char *TAG = "ble";

// =============================================================================
// Omi 128-bit UUID
// NimBLE 的 BLE_UUID128_INIT 按低字节在前的顺序填写。
// =============================================================================
static const ble_uuid128_t OMI_SERVICE_UUID = BLE_UUID128_INIT(
    0x14, 0x12, 0x8A, 0x76, 0x04, 0xD1, 0x6C, 0x4F,
    0x7E, 0x53, 0xF2, 0xE8, 0x00, 0x00, 0xB1, 0x19);
static const ble_uuid128_t AUDIO_DATA_UUID = BLE_UUID128_INIT(
    0x14, 0x12, 0x8A, 0x76, 0x04, 0xD1, 0x6C, 0x4F,
    0x7E, 0x53, 0xF2, 0xE8, 0x01, 0x00, 0xB1, 0x19);
static const ble_uuid128_t AUDIO_CODEC_UUID = BLE_UUID128_INIT(
    0x14, 0x12, 0x8A, 0x76, 0x04, 0xD1, 0x6C, 0x4F,
    0x7E, 0x53, 0xF2, 0xE8, 0x02, 0x00, 0xB1, 0x19);
static const ble_uuid128_t PHOTO_DATA_UUID = BLE_UUID128_INIT(
    0x14, 0x12, 0x8A, 0x76, 0x04, 0xD1, 0x6C, 0x4F,
    0x7E, 0x53, 0xF2, 0xE8, 0x05, 0x00, 0xB1, 0x19);
static const ble_uuid128_t PHOTO_CONTROL_UUID = BLE_UUID128_INIT(
    0x14, 0x12, 0x8A, 0x76, 0x04, 0xD1, 0x6C, 0x4F,
    0x7E, 0x53, 0xF2, 0xE8, 0x06, 0x00, 0xB1, 0x19);

static const ble_uuid128_t OTA_SERVICE_UUID = BLE_UUID128_INIT(
    0x14, 0x12, 0x8A, 0x76, 0x04, 0xD1, 0x6C, 0x4F,
    0x7E, 0x53, 0xF2, 0xE8, 0x10, 0x00, 0xB1, 0x19);
static const ble_uuid128_t OTA_CONTROL_UUID = BLE_UUID128_INIT(
    0x14, 0x12, 0x8A, 0x76, 0x04, 0xD1, 0x6C, 0x4F,
    0x7E, 0x53, 0xF2, 0xE8, 0x11, 0x00, 0xB1, 0x19);
static const ble_uuid128_t OTA_DATA_UUID = BLE_UUID128_INIT(
    0x14, 0x12, 0x8A, 0x76, 0x04, 0xD1, 0x6C, 0x4F,
    0x7E, 0x53, 0xF2, 0xE8, 0x12, 0x00, 0xB1, 0x19);

// 标准 Bluetooth SIG UUID。
static const ble_uuid16_t BATTERY_SERVICE_UUID = BLE_UUID16_INIT(0x180F);
static const ble_uuid16_t BATTERY_LEVEL_UUID = BLE_UUID16_INIT(0x2A19);
static const ble_uuid16_t DEVICE_INFO_SERVICE_UUID = BLE_UUID16_INIT(0x180A);
static const ble_uuid16_t MANUFACTURER_NAME_UUID = BLE_UUID16_INIT(0x2A29);
static const ble_uuid16_t MODEL_NUMBER_UUID = BLE_UUID16_INIT(0x2A24);
static const ble_uuid16_t FIRMWARE_REVISION_UUID = BLE_UUID16_INIT(0x2A26);
static const ble_uuid16_t HARDWARE_REVISION_UUID = BLE_UUID16_INIT(0x2A27);
static const ble_uuid16_t SERIAL_NUMBER_UUID = BLE_UUID16_INIT(0x2A25);

enum class CharacteristicId : uintptr_t {
    AUDIO_DATA = 1,
    AUDIO_CODEC,
    PHOTO_DATA,
    PHOTO_CONTROL,
    BATTERY_LEVEL,
    MANUFACTURER_NAME,
    MODEL_NUMBER,
    FIRMWARE_REVISION,
    HARDWARE_REVISION,
    SERIAL_NUMBER,
    OTA_CONTROL,
    OTA_DATA,
};

std::atomic_bool s_ready{false};
std::atomic_bool s_connected{false};
std::atomic_bool s_audio_subscribed{false};
std::atomic_bool s_photo_subscribed{false};
std::atomic_bool s_battery_subscribed{false};
std::atomic_bool s_ota_subscribed{false};
std::atomic<uint16_t> s_mtu{23};
std::atomic<uint16_t> s_conn_handle{BLE_HS_CONN_HANDLE_NONE};

// Photo Control 回调发生在 NimBLE Host Task。
// 这里先把命令保存成原子状态，再由 app_main 所在线程取走处理，避免跨任务直接操作 Camera。
std::atomic_bool s_photo_control_pending{false};
std::atomic<int> s_photo_control_command{0};

// Battery Level 会同时被业务主循环更新、被 NimBLE Host Task 读取，因此使用原子值避免数据竞争。
std::atomic<uint8_t> s_battery_level{0};

uint8_t s_own_addr_type = 0;
std::array<char, 17> s_device_serial{};

uint16_t s_audio_data_handle = 0;
uint16_t s_photo_data_handle = 0;
uint16_t s_battery_level_handle = 0;
uint16_t s_ota_data_handle = 0;

ble_gatt_chr_def s_main_characteristics[5]{};
ble_gatt_chr_def s_battery_characteristics[2]{};
ble_gatt_chr_def s_device_info_characteristics[6]{};
ble_gatt_chr_def s_ota_characteristics[3]{};
ble_gatt_svc_def s_services[5]{};

void *characteristic_arg(CharacteristicId id)
{
    return reinterpret_cast<void *>(static_cast<uintptr_t>(id));
}

CharacteristicId characteristic_id(void *arg)
{
    return static_cast<CharacteristicId>(reinterpret_cast<uintptr_t>(arg));
}

int append_value(struct os_mbuf *om, const void *data, size_t len)
{
    if (len == 0) {
        return 0;
    }
    return os_mbuf_append(om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int append_string(struct os_mbuf *om, const char *value)
{
    return append_value(om, value, std::strlen(value));
}

int handle_characteristic_read(struct ble_gatt_access_ctxt *ctxt, CharacteristicId id)
{
    switch (id) {
    case CharacteristicId::AUDIO_DATA:
        // Audio Data 的主要用途是 Notify；读取时保持空值，兼容原 GATT 权限。
        return 0;

    case CharacteristicId::AUDIO_CODEC: {
        const uint8_t codec_id = OMI_AUDIO_CODEC_ID;
        return append_value(ctxt->om, &codec_id, sizeof(codec_id));
    }

    case CharacteristicId::PHOTO_DATA:
        // Photo Data 的主要用途是 Notify；读取时保持空值，兼容原 GATT 权限。
        return 0;

    case CharacteristicId::BATTERY_LEVEL: {
        const uint8_t battery_level = s_battery_level.load();
        return append_value(ctxt->om, &battery_level, sizeof(battery_level));
    }

    case CharacteristicId::MANUFACTURER_NAME:
        return append_string(ctxt->om, OMI_MANUFACTURER_NAME);

    case CharacteristicId::MODEL_NUMBER:
        return append_string(ctxt->om, OMI_BLE_DEVICE_NAME);

    case CharacteristicId::FIRMWARE_REVISION:
        return append_string(ctxt->om, OMI_FIRMWARE_VERSION_STRING);

    case CharacteristicId::HARDWARE_REVISION:
        return append_string(ctxt->om, OMI_HARDWARE_REVISION);

    case CharacteristicId::SERIAL_NUMBER:
        return append_string(ctxt->om, s_device_serial.data());

    case CharacteristicId::OTA_CONTROL: {
        // OTA 逻辑尚未迁移，先保持协议可读，状态定义为 IDLE。
        const uint8_t status[2] = {0x00, 0x00};
        return append_value(ctxt->om, status, sizeof(status));
    }

    case CharacteristicId::OTA_DATA: {
        const uint8_t status = 0x00;
        return append_value(ctxt->om, &status, sizeof(status));
    }

    case CharacteristicId::PHOTO_CONTROL:
        break;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

int handle_characteristic_write(struct ble_gatt_access_ctxt *ctxt, CharacteristicId id)
{
    const uint16_t packet_len = OS_MBUF_PKTLEN(ctxt->om);

    if (id == CharacteristicId::PHOTO_CONTROL) {
        if (packet_len != 1) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        uint8_t raw_value = 0;
        uint16_t copied = 0;
        const int rc = ble_hs_mbuf_to_flat(ctxt->om, &raw_value, 1, &copied);
        if (rc != 0 || copied != 1) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        const int8_t signed_value = static_cast<int8_t>(raw_value);
        s_photo_control_command.store(static_cast<int>(signed_value));
        s_photo_control_pending.store(true);

        ESP_LOGI(TAG, "收到 Photo Control=%d，已交给业务主循环处理", static_cast<int>(signed_value));
        return 0;
    }

    if (id == CharacteristicId::OTA_CONTROL) {
        if (packet_len == 0 || packet_len > 512) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        std::array<uint8_t, 512> buffer{};
        uint16_t copied = 0;
        const int rc = ble_hs_mbuf_to_flat(ctxt->om, buffer.data(), packet_len, &copied);
        if (rc != 0 || copied != packet_len) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        // Phase 10 再接入真实 Wi-Fi / HTTPS OTA。
        ESP_LOGI(TAG, "收到 OTA Control 命令 0x%02X，长度=%u；当前仅记录不执行", buffer[0], copied);
        return 0;
    }

    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
}

int gatt_access_callback(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg)
{
    const CharacteristicId id = characteristic_id(arg);

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        ESP_LOGD(TAG, "GATT Read：conn=%u attr=%u", conn_handle, attr_handle);
        return handle_characteristic_read(ctxt, id);

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        ESP_LOGD(TAG, "GATT Write：conn=%u attr=%u", conn_handle, attr_handle);
        return handle_characteristic_write(ctxt, id);

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

void configure_characteristic(
    ble_gatt_chr_def &definition,
    const ble_uuid_t *uuid,
    CharacteristicId id,
    uint16_t flags,
    uint16_t *value_handle)
{
    definition.uuid = uuid;
    definition.access_cb = gatt_access_callback;
    definition.arg = characteristic_arg(id);
    definition.flags = flags;
    definition.val_handle = value_handle;
}

void build_gatt_table()
{
    std::memset(s_main_characteristics, 0, sizeof(s_main_characteristics));
    std::memset(s_battery_characteristics, 0, sizeof(s_battery_characteristics));
    std::memset(s_device_info_characteristics, 0, sizeof(s_device_info_characteristics));
    std::memset(s_ota_characteristics, 0, sizeof(s_ota_characteristics));
    std::memset(s_services, 0, sizeof(s_services));

    configure_characteristic(
        s_main_characteristics[0],
        &AUDIO_DATA_UUID.u,
        CharacteristicId::AUDIO_DATA,
        BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        &s_audio_data_handle);
    configure_characteristic(
        s_main_characteristics[1],
        &AUDIO_CODEC_UUID.u,
        CharacteristicId::AUDIO_CODEC,
        BLE_GATT_CHR_F_READ,
        nullptr);
    configure_characteristic(
        s_main_characteristics[2],
        &PHOTO_DATA_UUID.u,
        CharacteristicId::PHOTO_DATA,
        BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        &s_photo_data_handle);
    configure_characteristic(
        s_main_characteristics[3],
        &PHOTO_CONTROL_UUID.u,
        CharacteristicId::PHOTO_CONTROL,
        BLE_GATT_CHR_F_WRITE,
        nullptr);

    configure_characteristic(
        s_battery_characteristics[0],
        &BATTERY_LEVEL_UUID.u,
        CharacteristicId::BATTERY_LEVEL,
        BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        &s_battery_level_handle);

    configure_characteristic(
        s_device_info_characteristics[0],
        &MANUFACTURER_NAME_UUID.u,
        CharacteristicId::MANUFACTURER_NAME,
        BLE_GATT_CHR_F_READ,
        nullptr);
    configure_characteristic(
        s_device_info_characteristics[1],
        &MODEL_NUMBER_UUID.u,
        CharacteristicId::MODEL_NUMBER,
        BLE_GATT_CHR_F_READ,
        nullptr);
    configure_characteristic(
        s_device_info_characteristics[2],
        &FIRMWARE_REVISION_UUID.u,
        CharacteristicId::FIRMWARE_REVISION,
        BLE_GATT_CHR_F_READ,
        nullptr);
    configure_characteristic(
        s_device_info_characteristics[3],
        &HARDWARE_REVISION_UUID.u,
        CharacteristicId::HARDWARE_REVISION,
        BLE_GATT_CHR_F_READ,
        nullptr);
    configure_characteristic(
        s_device_info_characteristics[4],
        &SERIAL_NUMBER_UUID.u,
        CharacteristicId::SERIAL_NUMBER,
        BLE_GATT_CHR_F_READ,
        nullptr);

    configure_characteristic(
        s_ota_characteristics[0],
        &OTA_CONTROL_UUID.u,
        CharacteristicId::OTA_CONTROL,
        BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        nullptr);
    configure_characteristic(
        s_ota_characteristics[1],
        &OTA_DATA_UUID.u,
        CharacteristicId::OTA_DATA,
        BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        &s_ota_data_handle);

    s_services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    s_services[0].uuid = &OMI_SERVICE_UUID.u;
    s_services[0].characteristics = s_main_characteristics;

    s_services[1].type = BLE_GATT_SVC_TYPE_PRIMARY;
    s_services[1].uuid = &BATTERY_SERVICE_UUID.u;
    s_services[1].characteristics = s_battery_characteristics;

    s_services[2].type = BLE_GATT_SVC_TYPE_PRIMARY;
    s_services[2].uuid = &DEVICE_INFO_SERVICE_UUID.u;
    s_services[2].characteristics = s_device_info_characteristics;

    s_services[3].type = BLE_GATT_SVC_TYPE_PRIMARY;
    s_services[3].uuid = &OTA_SERVICE_UUID.u;
    s_services[3].characteristics = s_ota_characteristics;
}

void reset_subscription_state()
{
    s_audio_subscribed.store(false);
    s_photo_subscribed.store(false);
    s_battery_subscribed.store(false);
    s_ota_subscribed.store(false);
}

void update_connection_parameters(uint16_t conn_handle)
{
    ble_gap_upd_params params{};
    params.itvl_min = OMI_BLE_CONN_MIN_INTERVAL;
    params.itvl_max = OMI_BLE_CONN_MAX_INTERVAL;
    params.latency = OMI_BLE_CONN_LATENCY;
    params.supervision_timeout = OMI_BLE_CONN_TIMEOUT;
    params.min_ce_len = 0;
    params.max_ce_len = 0;

    const int rc = ble_gap_update_params(conn_handle, &params);
    if (rc != 0) {
        ESP_LOGW(TAG, "请求更新连接参数失败，rc=%d", rc);
    } else {
        ESP_LOGI(
            TAG,
            "已请求连接参数：interval=%u~%u (1.25ms)，latency=%u，timeout=%u (10ms)",
            OMI_BLE_CONN_MIN_INTERVAL,
            OMI_BLE_CONN_MAX_INTERVAL,
            OMI_BLE_CONN_LATENCY,
            OMI_BLE_CONN_TIMEOUT);
    }
}

int start_advertising();

int gap_event_callback(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connected.store(true);
            s_conn_handle.store(event->connect.conn_handle);
            s_mtu.store(23);
            reset_subscription_state();
            ESP_LOGI(TAG, "BLE 已连接，conn_handle=%u", event->connect.conn_handle);
            update_connection_parameters(event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "BLE 连接失败，status=%d，重新广播", event->connect.status);
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE 已断开，reason=%d，重新广播", event->disconnect.reason);
        s_connected.store(false);
        s_conn_handle.store(BLE_HS_CONN_HANDLE_NONE);
        s_mtu.store(23);
        reset_subscription_state();
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "BLE 广播结束，重新开始广播");
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE: {
        const bool notify_enabled = event->subscribe.cur_notify == 1;
        if (event->subscribe.attr_handle == s_audio_data_handle) {
            s_audio_subscribed.store(notify_enabled);
            ESP_LOGI(TAG, "Audio Data 通知订阅=%s", notify_enabled ? "开启" : "关闭");
        } else if (event->subscribe.attr_handle == s_photo_data_handle) {
            s_photo_subscribed.store(notify_enabled);
            ESP_LOGI(TAG, "Photo Data 通知订阅=%s", notify_enabled ? "开启" : "关闭");
        } else if (event->subscribe.attr_handle == s_battery_level_handle) {
            s_battery_subscribed.store(notify_enabled);
            ESP_LOGI(TAG, "Battery 通知订阅=%s", notify_enabled ? "开启" : "关闭");
        } else if (event->subscribe.attr_handle == s_ota_data_handle) {
            s_ota_subscribed.store(notify_enabled);
            ESP_LOGI(TAG, "OTA Data 通知订阅=%s", notify_enabled ? "开启" : "关闭");
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        s_mtu.store(event->mtu.value);
        ESP_LOGI(TAG, "ATT MTU 更新：conn=%u mtu=%u", event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "BLE 连接参数已更新，status=%d", event->conn_update.status);
        return 0;

    default:
        return 0;
    }
}

int start_advertising()
{
    ble_hs_adv_fields adv_fields{};
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.uuids128 = &OMI_SERVICE_UUID;
    adv_fields.num_uuids128 = 1;
    adv_fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播数据失败，rc=%d", rc);
        return rc;
    }

    // 与原 Arduino setScanResponse(true) 保持思路一致：设备名放到 Scan Response。
    ble_hs_adv_fields scan_rsp{};
    scan_rsp.name = reinterpret_cast<uint8_t *>(const_cast<char *>(OMI_BLE_DEVICE_NAME));
    scan_rsp.name_len = std::strlen(OMI_BLE_DEVICE_NAME);
    scan_rsp.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&scan_rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置 Scan Response 失败，rc=%d", rc);
        return rc;
    }

    ble_gap_adv_params adv_params{};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = OMI_BLE_ADV_MIN_INTERVAL;
    adv_params.itvl_max = OMI_BLE_ADV_MAX_INTERVAL;

    rc = ble_gap_adv_start(
        s_own_addr_type,
        nullptr,
        BLE_HS_FOREVER,
        &adv_params,
        gap_event_callback,
        nullptr);

    if (rc != 0) {
        ESP_LOGE(TAG, "启动 BLE 广播失败，rc=%d", rc);
        return rc;
    }

    ESP_LOGI(TAG, "BLE 广播已启动，设备名=%s", OMI_BLE_DEVICE_NAME);
    return 0;
}

void on_stack_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE Host reset，reason=%d", reason);
    s_ready.store(false);
    s_connected.store(false);
    s_conn_handle.store(BLE_HS_CONN_HANDLE_NONE);
    s_mtu.store(23);
    reset_subscription_state();
}

void on_stack_sync()
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "准备 BLE 地址失败，rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "推断 BLE 地址类型失败，rc=%d", rc);
        return;
    }

    s_ready.store(true);
    ESP_LOGI(TAG, "NimBLE Host 已同步，开始广播");
    start_advertising();
}

void gatt_register_callback(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)arg;
    char uuid_str[BLE_UUID_STR_LEN]{};

    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGI(
            TAG,
            "注册 Service：%s handle=%u",
            ble_uuid_to_str(ctxt->svc.svc_def->uuid, uuid_str),
            ctxt->svc.handle);
        break;

    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGI(
            TAG,
            "注册 Characteristic：%s value_handle=%u",
            ble_uuid_to_str(ctxt->chr.chr_def->uuid, uuid_str),
            ctxt->chr.val_handle);
        break;

    case BLE_GATT_REGISTER_OP_DSC:
        ESP_LOGD(
            TAG,
            "注册 Descriptor：%s handle=%u",
            ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, uuid_str),
            ctxt->dsc.handle);
        break;

    default:
        break;
    }
}

void nimble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE Host Task 启动");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t initialize_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新初始化，先擦除 NVS 分区");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t notify_characteristic(uint16_t value_handle, bool subscribed, const uint8_t *data, size_t len)
{
    if (!s_connected.load() || s_conn_handle.load() == BLE_HS_CONN_HANDLE_NONE || !subscribed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (value_handle == 0 || data == nullptr || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // ATT Notification 的 Attribute Value 上限为 MTU - 3。
    if (s_mtu.load() < len + 3) {
        return ESP_ERR_INVALID_SIZE;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // ble_gatts_notify_custom() 会接管 om 的生命周期；无论成功失败都不能再次手工 free。
    const int rc = ble_gatts_notify_custom(s_conn_handle.load(), value_handle, om);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

} // namespace

esp_err_t ble_service_init(const char *device_serial)
{
    if (device_serial == nullptr || device_serial[0] == '\0') {
        ESP_LOGE(TAG, "BLE 初始化需要有效的设备序列号");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ready.load()) {
        return ESP_OK;
    }

    std::strncpy(s_device_serial.data(), device_serial, s_device_serial.size() - 1);
    s_device_serial.back() = '\0';

    ESP_LOGI(TAG, "初始化 ESP-IDF v6.1 原生 NimBLE GATT Server");

    esp_err_t err = initialize_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE Stack 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    build_gatt_table();

    // 初始化标准 GAP/GATT 服务，再注册 Omi 自定义服务。
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_svc_gap_device_name_set(OMI_BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置 BLE Device Name 失败，rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    rc = ble_att_set_preferred_mtu(OMI_BLE_MTU_SIZE);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置 Preferred MTU=%u 失败，rc=%d", OMI_BLE_MTU_SIZE, rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    rc = ble_gatts_count_cfg(s_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "统计 GATT 表资源失败，rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "注册 GATT 服务失败，rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb = on_stack_sync;
    ble_hs_cfg.gatts_register_cb = gatt_register_callback;

    // NimBLE Host 在独立 FreeRTOS Task 中运行，业务主循环无需轮询 BLE 栈。
    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "BLE Core 已启动，等待 Host sync 后开始广播");
    return ESP_OK;
}

bool ble_service_is_ready()
{
    return s_ready.load();
}

bool ble_service_is_connected()
{
    return s_connected.load();
}

bool ble_service_audio_subscribed()
{
    return s_audio_subscribed.load();
}

bool ble_service_photo_subscribed()
{
    return s_photo_subscribed.load();
}

bool ble_service_battery_subscribed()
{
    return s_battery_subscribed.load();
}

bool ble_service_ota_subscribed()
{
    return s_ota_subscribed.load();
}

uint16_t ble_service_mtu()
{
    return s_mtu.load();
}

esp_err_t ble_service_notify_audio(const uint8_t *data, size_t len)
{
    return notify_characteristic(s_audio_data_handle, s_audio_subscribed.load(), data, len);
}

esp_err_t ble_service_notify_photo(const uint8_t *data, size_t len)
{
    return notify_characteristic(s_photo_data_handle, s_photo_subscribed.load(), data, len);
}

bool ble_service_take_photo_control(int8_t *value)
{
    if (value == nullptr || !s_photo_control_pending.exchange(false)) {
        return false;
    }

    *value = static_cast<int8_t>(s_photo_control_command.load());
    return true;
}

esp_err_t ble_service_update_battery_level(uint8_t percentage, bool notify_if_subscribed)
{
    if (percentage > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    s_battery_level.store(percentage);

    // 即使当前没有连接或没有订阅，也已经更新 GATT Read 会返回的最新值。
    if (!notify_if_subscribed || !s_connected.load() || !s_battery_subscribed.load()) {
        return ESP_OK;
    }

    return notify_characteristic(
        s_battery_level_handle,
        true,
        &percentage,
        sizeof(percentage));
}
