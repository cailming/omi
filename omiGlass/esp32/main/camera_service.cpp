#include "camera_service.h"

#include "esp_camera.h"
#include "esp_log.h"
#include "omiglass_config.h"

namespace {

static const char *TAG = "camera";

bool s_ready = false;
camera_fb_t *s_frame = nullptr;

camera_config_t build_camera_config()
{
    camera_config_t config{};

    // XIAO ESP32S3 Sense Camera 引脚映射，保持与原 Arduino 固件一致。
    config.pin_pwdn = OMI_CAMERA_PWDN_GPIO;
    config.pin_reset = OMI_CAMERA_RESET_GPIO;
    config.pin_xclk = OMI_CAMERA_XCLK_GPIO;
    config.pin_sccb_sda = OMI_CAMERA_SIOD_GPIO;
    config.pin_sccb_scl = OMI_CAMERA_SIOC_GPIO;

    config.pin_d7 = OMI_CAMERA_Y9_GPIO;
    config.pin_d6 = OMI_CAMERA_Y8_GPIO;
    config.pin_d5 = OMI_CAMERA_Y7_GPIO;
    config.pin_d4 = OMI_CAMERA_Y6_GPIO;
    config.pin_d3 = OMI_CAMERA_Y5_GPIO;
    config.pin_d2 = OMI_CAMERA_Y4_GPIO;
    config.pin_d1 = OMI_CAMERA_Y3_GPIO;
    config.pin_d0 = OMI_CAMERA_Y2_GPIO;

    config.pin_vsync = OMI_CAMERA_VSYNC_GPIO;
    config.pin_href = OMI_CAMERA_HREF_GPIO;
    config.pin_pclk = OMI_CAMERA_PCLK_GPIO;

    // 与原固件保持 6MHz XCLK。
    config.xclk_freq_hz = OMI_CAMERA_XCLK_HZ;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;

    // Phase 6 固定使用 VGA JPEG，不改变手机端后续图片协议预期。
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = OMI_CAMERA_JPEG_QUALITY;
    config.fb_count = OMI_CAMERA_FRAME_BUFFER_COUNT;

    // framebuffer 放到 PSRAM，保持内部 SRAM 给 BLE / Audio 等实时模块使用。
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;

    // 使用驱动内部 SCCB 总线；jpeg_buffer_size=0 让组件使用默认策略。
    config.sccb_i2c_port = 0;
    config.jpeg_buffer_size = 0;

    return config;
}

} // namespace

esp_err_t camera_service_init()
{
    if (s_ready) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化 XIAO ESP32S3 Sense Camera");
    ESP_LOGI(
        TAG,
        "Camera 参数：VGA JPEG，quality=%d，XCLK=%luHz，framebuffer=%u",
        OMI_CAMERA_JPEG_QUALITY,
        static_cast<unsigned long>(OMI_CAMERA_XCLK_HZ),
        static_cast<unsigned>(OMI_CAMERA_FRAME_BUFFER_COUNT));

    camera_config_t config = build_camera_config();
    const esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera 初始化失败: %s (0x%x)", esp_err_to_name(err), err);
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "Camera 初始化成功");
    return ESP_OK;
}

void camera_service_deinit()
{
    camera_service_release_frame();

    if (!s_ready) {
        return;
    }

    const esp_err_t err = esp_camera_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Camera deinit 返回: %s", esp_err_to_name(err));
    }

    s_ready = false;
}

bool camera_service_is_ready()
{
    return s_ready;
}

esp_err_t camera_service_capture(CameraFrameView *view)
{
    if (view == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *view = {};

    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    // 与原 Arduino 实现一致：新拍照前先归还上一帧，避免 framebuffer 被长期占用。
    camera_service_release_frame();

    s_frame = esp_camera_fb_get();
    if (s_frame == nullptr) {
        ESP_LOGE(TAG, "无法获取 Camera framebuffer");
        return ESP_FAIL;
    }

    if (s_frame->format != PIXFORMAT_JPEG) {
        ESP_LOGW(TAG, "Camera 返回的帧格式不是 JPEG，format=%d", static_cast<int>(s_frame->format));
    }

    view->data = s_frame->buf;
    view->len = s_frame->len;
    view->width = s_frame->width;
    view->height = s_frame->height;
    view->orientation = OMI_CAMERA_FIXED_ORIENTATION;

    ESP_LOGI(
        TAG,
        "拍照成功：%ux%u，JPEG=%u bytes，orientation=%u(180°)",
        static_cast<unsigned>(view->width),
        static_cast<unsigned>(view->height),
        static_cast<unsigned>(view->len),
        static_cast<unsigned>(view->orientation));

    return ESP_OK;
}

void camera_service_release_frame()
{
    if (s_frame == nullptr) {
        return;
    }

    esp_camera_fb_return(s_frame);
    s_frame = nullptr;
}

bool camera_service_has_frame()
{
    return s_frame != nullptr;
}
