#include "mic_service.h"

#include <cstdlib>

#include "driver/i2s_pdm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "omiglass_config.h"

namespace {

static const char *TAG = "mic";

i2s_chan_handle_t s_rx_handle = nullptr;
int16_t *s_read_buffer = nullptr;
MicDataHandler s_audio_callback = nullptr;
bool s_running = false;

esp_err_t allocate_read_buffer()
{
    if (s_read_buffer != nullptr) {
        return ESP_OK;
    }

    const size_t buffer_bytes = OMI_MIC_BUFFER_SAMPLES * sizeof(int16_t);

    // 优先使用 PSRAM，保持与原 Arduino 固件一致，并减少内部 SRAM 压力。
    s_read_buffer = static_cast<int16_t *>(heap_caps_malloc(buffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_read_buffer != nullptr) {
        ESP_LOGI(TAG, "麦克风读取缓冲区使用 PSRAM，共 %u 字节", static_cast<unsigned>(buffer_bytes));
        return ESP_OK;
    }

    // 如果 PSRAM 不可用，则回退到普通可访问内存。
    s_read_buffer = static_cast<int16_t *>(heap_caps_malloc(buffer_bytes, MALLOC_CAP_8BIT));
    if (s_read_buffer == nullptr) {
        ESP_LOGE(TAG, "无法分配麦克风读取缓冲区");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGW(TAG, "PSRAM 分配失败，麦克风读取缓冲区回退到普通 RAM");
    return ESP_OK;
}

void free_read_buffer()
{
    if (s_read_buffer != nullptr) {
        heap_caps_free(s_read_buffer);
        s_read_buffer = nullptr;
    }
}

void apply_gain(int16_t *data, size_t samples)
{
    if (OMI_MIC_GAIN == 1) {
        return;
    }

    for (size_t i = 0; i < samples; ++i) {
        int32_t sample = static_cast<int32_t>(data[i]) * OMI_MIC_GAIN;

        // 与原 Arduino 实现一致，对增益后的结果做 16-bit 饱和处理。
        if (sample > 32767) {
            sample = 32767;
        } else if (sample < -32768) {
            sample = -32768;
        }

        data[i] = static_cast<int16_t>(sample);
    }
}

} // namespace

esp_err_t mic_service_start()
{
    if (s_running) {
        ESP_LOGW(TAG, "麦克风已经启动");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化 ESP-IDF 原生 PDM RX 麦克风");
    ESP_LOGI(TAG, "PDM CLK: GPIO%d，DATA: GPIO%d，采样率: %u Hz",
             OMI_MIC_CLK_GPIO,
             OMI_MIC_DATA_GPIO,
             static_cast<unsigned>(OMI_MIC_SAMPLE_RATE));

    esp_err_t err = allocate_read_buffer();
    if (err != ESP_OK) {
        return err;
    }

    // 为 I2S0 分配一个纯 RX 通道。ESP32-S3 的 PDM 麦克风只需要接收方向。
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = OMI_MIC_DMA_DESC_NUM;
    channel_config.dma_frame_num = OMI_MIC_DMA_FRAME_NUM;

    err = i2s_new_channel(&channel_config, nullptr, &s_rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建 I2S RX 通道失败: %s", esp_err_to_name(err));
        free_read_buffer();
        return err;
    }

    // ESP32-S3 支持硬件 PDM -> PCM 转换，因此直接配置成 16-bit 单声道 PCM 输出。
    i2s_pdm_rx_config_t pdm_config{};
    pdm_config.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(OMI_MIC_SAMPLE_RATE);
    pdm_config.slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    pdm_config.gpio_cfg.clk = OMI_MIC_CLK_GPIO;
    pdm_config.gpio_cfg.din = OMI_MIC_DATA_GPIO;
    pdm_config.gpio_cfg.invert_flags.clk_inv = false;

    err = i2s_channel_init_pdm_rx_mode(s_rx_handle, &pdm_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "初始化 PDM RX 模式失败: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_handle);
        s_rx_handle = nullptr;
        free_read_buffer();
        return err;
    }

    err = i2s_channel_enable(s_rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动 I2S RX 通道失败: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_handle);
        s_rx_handle = nullptr;
        free_read_buffer();
        return err;
    }

    s_running = true;
    ESP_LOGI(TAG, "PDM 麦克风启动成功");
    return ESP_OK;
}

void mic_service_stop()
{
    if (!s_running && s_rx_handle == nullptr) {
        free_read_buffer();
        return;
    }

    ESP_LOGI(TAG, "停止 PDM 麦克风");

    if (s_rx_handle != nullptr) {
        if (s_running) {
            const esp_err_t disable_err = i2s_channel_disable(s_rx_handle);
            if (disable_err != ESP_OK) {
                ESP_LOGW(TAG, "停止 I2S RX 通道时返回: %s", esp_err_to_name(disable_err));
            }
        }

        const esp_err_t delete_err = i2s_del_channel(s_rx_handle);
        if (delete_err != ESP_OK) {
            ESP_LOGW(TAG, "释放 I2S RX 通道时返回: %s", esp_err_to_name(delete_err));
        }
        s_rx_handle = nullptr;
    }

    free_read_buffer();
    s_running = false;
}

bool mic_service_is_running()
{
    return s_running;
}

void mic_service_set_callback(MicDataHandler callback)
{
    s_audio_callback = callback;
}

void mic_service_process()
{
    if (!s_running || s_rx_handle == nullptr || s_read_buffer == nullptr) {
        return;
    }

    size_t bytes_read = 0;
    const size_t requested_bytes = OMI_MIC_READ_SAMPLES * sizeof(int16_t);

    const esp_err_t err = i2s_channel_read(
        s_rx_handle,
        s_read_buffer,
        requested_bytes,
        &bytes_read,
        OMI_MIC_READ_TIMEOUT_MS);

    // 超时并不代表设备故障，下一轮继续读取即可。
    if (err == ESP_ERR_TIMEOUT) {
        return;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取 PDM PCM 数据失败: %s", esp_err_to_name(err));
        return;
    }

    if (bytes_read == 0) {
        return;
    }

    const size_t samples_read = bytes_read / sizeof(int16_t);
    apply_gain(s_read_buffer, samples_read);

    if (s_audio_callback != nullptr) {
        s_audio_callback(s_read_buffer, samples_read);
    }
}
