#include "opus_service.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "omiglass_config.h"
#include "opus.h"

namespace {

static const char *TAG = "opus";

OpusEncoder *s_encoder = nullptr;
OpusEncodedHandler s_encoded_callback = nullptr;

int16_t *s_pcm_ring_buffer = nullptr;
int16_t *s_opus_input_buffer = nullptr;
uint8_t *s_opus_output_buffer = nullptr;

size_t s_ring_write_pos = 0;
size_t s_ring_read_pos = 0;
bool s_ready = false;

void *allocate_audio_buffer(size_t bytes)
{
    // 优先使用 PSRAM，保持原 Arduino 固件的内存策略，减少内部 SRAM 压力。
    void *ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr != nullptr) {
        return ptr;
    }

    // 如果 PSRAM 暂时不可用，则回退到普通 8-bit 可访问内存，便于调试阶段继续运行。
    return heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

void free_audio_buffers()
{
    if (s_pcm_ring_buffer != nullptr) {
        heap_caps_free(s_pcm_ring_buffer);
        s_pcm_ring_buffer = nullptr;
    }

    if (s_opus_input_buffer != nullptr) {
        heap_caps_free(s_opus_input_buffer);
        s_opus_input_buffer = nullptr;
    }

    if (s_opus_output_buffer != nullptr) {
        heap_caps_free(s_opus_output_buffer);
        s_opus_output_buffer = nullptr;
    }
}

size_t ring_buffer_available()
{
    if (s_ring_write_pos >= s_ring_read_pos) {
        return s_ring_write_pos - s_ring_read_pos;
    }

    return OMI_AUDIO_RING_BUFFER_SAMPLES - s_ring_read_pos + s_ring_write_pos;
}

esp_err_t configure_encoder()
{
    struct ControlSetting {
        const char *name;
        int result;
    };

    const ControlSetting settings[] = {
        {"bitrate", opus_encoder_ctl(s_encoder, OPUS_SET_BITRATE(OMI_OPUS_BITRATE))},
        {"complexity", opus_encoder_ctl(s_encoder, OPUS_SET_COMPLEXITY(OMI_OPUS_COMPLEXITY))},
        {"signal", opus_encoder_ctl(s_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE))},
        {"vbr", opus_encoder_ctl(s_encoder, OPUS_SET_VBR(OMI_OPUS_VBR))},
        {"vbr_constraint", opus_encoder_ctl(s_encoder, OPUS_SET_VBR_CONSTRAINT(0))},
        {"lsb_depth", opus_encoder_ctl(s_encoder, OPUS_SET_LSB_DEPTH(16))},
        {"dtx", opus_encoder_ctl(s_encoder, OPUS_SET_DTX(0))},
        {"inband_fec", opus_encoder_ctl(s_encoder, OPUS_SET_INBAND_FEC(0))},
        {"packet_loss", opus_encoder_ctl(s_encoder, OPUS_SET_PACKET_LOSS_PERC(0))},
    };

    for (const auto &setting : settings) {
        if (setting.result != OPUS_OK) {
            ESP_LOGE(TAG, "设置 Opus 参数 %s 失败，错误码=%d", setting.name, setting.result);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

int encode_one_frame()
{
    if (s_encoder == nullptr || s_opus_input_buffer == nullptr || s_opus_output_buffer == nullptr) {
        return OPUS_INVALID_STATE;
    }

    for (size_t i = 0; i < OMI_OPUS_FRAME_SAMPLES; ++i) {
        s_opus_input_buffer[i] = s_pcm_ring_buffer[s_ring_read_pos];
        s_ring_read_pos = (s_ring_read_pos + 1) % OMI_AUDIO_RING_BUFFER_SAMPLES;
    }

    return opus_encode(
        s_encoder,
        s_opus_input_buffer,
        static_cast<int>(OMI_OPUS_FRAME_SAMPLES),
        s_opus_output_buffer,
        static_cast<opus_int32>(OMI_OPUS_OUTPUT_MAX_BYTES));
}

} // namespace

esp_err_t opus_service_init()
{
    if (s_ready) {
        ESP_LOGW(TAG, "Opus 编码器已经初始化");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化 Opus 编码器：%u Hz / mono / %d bps / %u samples",
             static_cast<unsigned>(OMI_MIC_SAMPLE_RATE),
             OMI_OPUS_BITRATE,
             static_cast<unsigned>(OMI_OPUS_FRAME_SAMPLES));

    s_pcm_ring_buffer = static_cast<int16_t *>(
        allocate_audio_buffer(OMI_AUDIO_RING_BUFFER_SAMPLES * sizeof(int16_t)));
    s_opus_input_buffer = static_cast<int16_t *>(
        allocate_audio_buffer(OMI_OPUS_FRAME_SAMPLES * sizeof(int16_t)));
    s_opus_output_buffer = static_cast<uint8_t *>(
        allocate_audio_buffer(OMI_OPUS_OUTPUT_MAX_BYTES));

    if (s_pcm_ring_buffer == nullptr || s_opus_input_buffer == nullptr || s_opus_output_buffer == nullptr) {
        ESP_LOGE(TAG, "Opus 缓冲区分配失败");
        free_audio_buffers();
        return ESP_ERR_NO_MEM;
    }

    int opus_error = OPUS_OK;
    s_encoder = opus_encoder_create(
        static_cast<opus_int32>(OMI_MIC_SAMPLE_RATE),
        1,
        OPUS_APPLICATION_VOIP,
        &opus_error);

    if (s_encoder == nullptr || opus_error != OPUS_OK) {
        ESP_LOGE(TAG, "创建 Opus 编码器失败，错误码=%d", opus_error);
        if (s_encoder != nullptr) {
            opus_encoder_destroy(s_encoder);
            s_encoder = nullptr;
        }
        free_audio_buffers();
        return ESP_FAIL;
    }

    const esp_err_t config_err = configure_encoder();
    if (config_err != ESP_OK) {
        opus_encoder_destroy(s_encoder);
        s_encoder = nullptr;
        free_audio_buffers();
        return config_err;
    }

    s_ring_write_pos = 0;
    s_ring_read_pos = 0;
    s_ready = true;

    ESP_LOGI(TAG, "Opus 编码器初始化成功，Codec ID=%u", OMI_AUDIO_CODEC_ID);
    return ESP_OK;
}

void opus_service_deinit()
{
    if (s_encoder != nullptr) {
        opus_encoder_destroy(s_encoder);
        s_encoder = nullptr;
    }

    free_audio_buffers();
    s_ring_write_pos = 0;
    s_ring_read_pos = 0;
    s_ready = false;
}

void opus_service_set_callback(OpusEncodedHandler callback)
{
    s_encoded_callback = callback;
}

esp_err_t opus_service_receive_pcm(const int16_t *data, size_t samples)
{
    if (!s_ready || s_pcm_ring_buffer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (data == nullptr && samples > 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < samples; ++i) {
        const size_t next_write = (s_ring_write_pos + 1) % OMI_AUDIO_RING_BUFFER_SAMPLES;

        if (next_write == s_ring_read_pos) {
            // 与原 Arduino 固件保持一致：缓冲区满时丢弃最旧 sample，而不是阻塞实时采集链路。
            s_ring_read_pos = (s_ring_read_pos + 1) % OMI_AUDIO_RING_BUFFER_SAMPLES;
        }

        s_pcm_ring_buffer[s_ring_write_pos] = data[i];
        s_ring_write_pos = next_write;
    }

    return ESP_OK;
}

void opus_service_process()
{
    if (!s_ready || s_encoder == nullptr) {
        return;
    }

    while (ring_buffer_available() >= OMI_OPUS_FRAME_SAMPLES) {
        const int encoded_bytes = encode_one_frame();

        if (encoded_bytes < 0) {
            ESP_LOGW(TAG, "Opus 编码失败，错误码=%d", encoded_bytes);
            continue;
        }

        if (encoded_bytes == 0) {
            continue;
        }

        if (static_cast<size_t>(encoded_bytes) > OMI_OPUS_OUTPUT_MAX_BYTES) {
            // 理论上 opus_encode() 不会超过传入的 max_data_bytes，这里保留防御性检查。
            ESP_LOGE(TAG, "Opus 输出异常：%d 字节，超过协议缓冲上限", encoded_bytes);
            continue;
        }

        if (s_encoded_callback != nullptr) {
            s_encoded_callback(s_opus_output_buffer, static_cast<size_t>(encoded_bytes));
        }
    }
}

bool opus_service_is_ready()
{
    return s_ready;
}

uint8_t opus_service_codec_id()
{
    return OMI_AUDIO_CODEC_ID;
}
