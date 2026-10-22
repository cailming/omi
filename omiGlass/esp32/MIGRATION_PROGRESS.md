# OmiGlass ESP-IDF 迁移进度

分支：`feature/omiglass-esp-idf-migration`

最后更新：2026-10-22

## 固定开发基线

- ESP-IDF：**v6.1.x**
- 版本约束：`>=6.1,<6.2`
- Target：ESP32-S3
- 硬件：Seeed Studio XIAO ESP32S3 Sense
- 原 Arduino 固件：`omiGlass/firmware/`，迁移期间保持不动
- 当前 Opus 组件：`78/esp-opus 1.0.5`
- 新增代码说明性注释统一使用中文

## 总体状态

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| Phase 0 | ESP-IDF 工程骨架 | 代码完成，待编译/真机验证 |
| Phase 1 | Board / GPIO / LED / Button | 代码完成，待硬件验证 |
| Phase 2 | PDM Mic | 代码完成，待硬件验证 |
| Phase 3 | Opus Encoder | 代码完成，待 v6.1 编译/真机验证 |
| Phase 4 | Native NimBLE / GATT | 代码完成，待手机 GATT 验证 |
| Phase 5 | BLE Audio TX | **代码完成，待手机实时音频验证** |
| Phase 6 | Camera | 未开始 |
| Phase 7 | Photo BLE | 未开始 |
| Phase 8 | Battery ADC | 未开始 |
| Phase 9 | Power | 未开始 |
| Phase 10 | OTA | 未开始 |
| Phase 11 | 整机功能对齐 | 未开始 |
| Phase 12 | FreeRTOS / Component 架构优化 | 未开始 |

---

## Phase 0：ESP-IDF 工程骨架

已完成：

- `omiGlass/esp32/` 原生 ESP-IDF 工程。
- ESP32-S3 / 8MB Flash / Octal PSRAM 基础配置。
- OTA 分区表按 Arduino 版本迁移。
- 使用 `app_main()` 作为程序入口。
- `main/idf_component.yml` 固定 ESP-IDF `>=6.1,<6.2`。
- GitHub Actions 增加 `omiglass-esp-idf.yml`，目标使用官方 ESP-IDF v6.1 环境执行构建。

当前 GitHub 连接器尚未返回该 workflow 的实际运行结果，因此不能声明 CI 已通过。

---

## Phase 1：板级基础能力

已迁移：

```text
GPIO1   电源按键，内部上拉，低电平按下
GPIO21  状态 LED，LOW=亮，HIGH=灭
```

Arduino API 替换：

```text
pinMode()          -> gpio_config()
digitalRead()      -> gpio_get_level()
digitalWrite()     -> gpio_set_level()
attachInterrupt()  -> GPIO ISR Service
millis()           -> esp_timer_get_time()
delay()            -> vTaskDelay()
Serial.*           -> ESP_LOGx
ESP.getEfuseMac()  -> esp_efuse_mac_get_default()
```

保留：

- 50ms 去抖。
- 2 秒长按识别。
- 启动 LED 闪烁。
- 原设备序列号生成语义。

---

## Phase 2：PDM 麦克风

原 Arduino legacy I2S 已迁移到 ESP-IDF v6.1 Channel API：

```text
GPIO42 PDM CLK
GPIO41 PDM DATA
16kHz
Mono
PCM16
```

调用链：

```text
i2s_new_channel()
  ↓
i2s_channel_init_pdm_rx_mode()
  ↓
i2s_channel_enable()
  ↓
i2s_channel_read()
```

当前每次读取：

```text
320 samples = 20ms
```

真机目标：约 `16000 samples/s`。

---

## Phase 3：Opus Encoder

新增：

- `main/opus_service.h/.cpp`
- `PHASE3_OPUS.md`

参数保持 Arduino 版本：

```text
16kHz
Mono
320 samples / 20ms
32kbps
Complexity 3
VBR ON
Codec ID 21
Max 160 bytes/frame
```

数据链：

```text
PCM
 ↓
8000-sample ring buffer
 ↓
320 samples/frame
 ↓
opus_encode()
 ↓
encoded callback
```

---

## Phase 4：Native NimBLE / GATT

新增：

- `main/ble_service.h/.cpp`
- `PHASE4_BLE_CORE.md`

已迁移：

- NimBLE Host。
- Peripheral / Broadcaster。
- Advertising / Scan Response。
- Connect / Disconnect。
- ATT MTU。
- Connection Parameters。
- CCCD / Subscribe 状态。
- Omi Main Service。
- Battery Service。
- Device Information Service。
- OTA Service。

GATT 主要结构保持：

```text
Omi Main Service
  Audio Data     READ + NOTIFY
  Audio Codec    READ
  Photo Data     READ + NOTIFY
  Photo Control  WRITE

Battery Service
Device Information Service
OTA Service
```

Preferred ATT MTU：517。

---

## Phase 5：BLE 实时音频发送

新增：

- `main/audio_tx_service.h`
- `main/audio_tx_service.cpp`
- `PHASE5_BLE_AUDIO.md`

BLE Core 新增：

```cpp
esp_err_t ble_service_notify_audio(const uint8_t *packet, size_t len);
```

BLE 层现在保存：

```text
当前 connection handle
Audio Data value handle
Audio Data Notify 订阅状态
当前 ATT MTU
```

### 手机端音频包格式保持不变

```text
byte 0-1：packet index，little-endian
byte 2  ：sub-index = 0
byte 3+ ：Opus payload
```

最大业务包：

```text
3 + 160 = 163 bytes
```

ATT Notification 需要：

```text
packet <= MTU - 3
```

因此最大 Opus 帧完整发送至少要求 MTU >= 166；项目目标 MTU 为 517。

### Audio TX Queue

队列容量：

```text
16 frames
```

即理论最大约：

```text
16 × 20ms = 320ms
```

实时策略：

- 未连接：不缓存音频。
- 未订阅 Audio Data Notify：不缓存音频。
- 队列满：丢最旧帧，保留最新帧。
- MTU 不足：丢当前旧帧，等待后续新帧。
- notify 失败：不持续重试旧帧，优先实时性。
- 每轮最多发送 2 帧，允许轻微追赶但避免一次塞满 BLE Host 队列。

### 当前完整音频链

```text
PDM Mic
  ↓
PCM16 16kHz
  ↓
Opus 20ms frame
  ↓
on_opus_encoded()
  ↓
audio_tx_service_enqueue()
  ↓
16-frame TX Queue
  ↓
3-byte Omi Header
  ↓
ble_service_notify_audio()
  ↓
ble_hs_mbuf_from_flat()
  ↓
ble_gatts_notify_custom()
  ↓
手机
```

NimBLE 的 `ble_gatts_notify_custom()` 会消费传入的 mbuf，因此发送后不能再次手工释放。

### 当前日志统计

每约一秒输出：

```text
麦克风：samples / peak
Opus：frames / bytes / avg / max
BLE Audio：
  subscribed
  MTU
  enqueued
  sent
  dropped_not_ready
  dropped_queue_full
  dropped_mtu_too_small
  notify_errors
  queue_depth
```

理想稳定状态：

```text
Mic samples/s       ≈ 16000
Opus frames/s       ≈ 50
BLE sent frames/s   ≈ 50
queue depth         ≈ 0
queue full drops    = 0
MTU drops           = 0
notify errors       = 0
```

---

## 当前 app_main 数据链

```text
app_main()
   │
   ├── board_service_init()
   ├── ble_service_init(serial)
   ├── audio_tx_service_init()
   ├── opus_service_init()
   ├── mic_service_start()
   │
   └── while(true)
          ├── BLE connected -> LED
          ├── board_service_process()
          ├── mic_service_process()
          ├── opus_service_process()
          │       ↓
          │   on_opus_encoded()
          │       ↓
          │   audio TX queue
          ├── audio_tx_service_process()
          ├── button event
          └── runtime statistics
```

---

## Phase 5 真机验证清单

1. 手机扫描到 `OMI Glass`。
2. 能成功连接。
3. Audio Codec 读取 `0x15`。
4. 手机订阅 Audio Data Notify。
5. MTU 协商到足够大小，目标 517。
6. Mic 每秒约 16000 samples。
7. Opus 每秒约 50 帧。
8. BLE Audio 每秒约发送 50 帧。
9. 手机收到的数据保持原有 3-byte header 格式。
10. packet index 连续递增并按 little-endian 解析。
11. sub-index 为 0。
12. 手机端原有 Opus 解码逻辑无需改协议即可工作。
13. 正常情况下 queue depth 长期接近 0。
14. queue full drops 不持续增长。
15. MTU drops 不持续增长。
16. notify errors 不持续增长。
17. 关闭 Notify 后不积压旧音频。
18. 重新开启 Notify 后从当前实时声音开始。
19. BLE 断开后待发音频队列清空。
20. 重连重新订阅后可恢复音频发送。

---

## 当前验证边界

当前聊天模式可以直接读写 GitHub，但没有直接连接：

- 本地 ESP-IDF v6.1 编译器。
- XIAO ESP32S3 Sense 真机。
- 手机 BLE 调试环境。

因此当前准确状态是：

> **Phase 0～5 的迁移代码已经完成。**

尚不能声明：

> `idf.py build` 已通过、真机已经成功烧录、BLE 音频已经实际播放成功。

本地验证：

```bash
cd omiGlass/esp32
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

---

## 下一步：Phase 6 Camera

Phase 6 只迁 Camera 本身，不发送照片：

```text
esp32-camera component
GPIO mapping
VGA JPEG
JPEG quality 25
XCLK 6MHz
PSRAM framebuffer
frame capture / release
180° orientation metadata
```

Camera 能稳定取到 JPEG framebuffer 后，再在 Phase 7 接 Photo BLE Transport。

---

## 固定兼容性约束

除非后续显式修改并记录：

- ESP-IDF 固定 v6.1.x。
- XIAO ESP32S3 Sense。
- BLE Device Name：`OMI Glass`。
- Omi BLE UUID 不变。
- Codec ID：21。
- 16kHz Mono。
- Opus 320 samples / 20ms。
- Audio packet 3-byte header 不变。
- Camera 基线：VGA JPEG。
- 手机端 BLE 协议不变。
- Arduino `omiGlass/firmware/` 保持不动。
