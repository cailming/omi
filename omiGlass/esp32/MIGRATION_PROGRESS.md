# OmiGlass ESP-IDF 迁移进度

分支：`feature/omiglass-esp-idf-migration`

最后更新：2026-10-22

## 固定开发基线

- ESP-IDF：**v6.1.x**
- 版本约束：`>=6.1,<6.2`
- Target：ESP32-S3
- 硬件：Seeed Studio XIAO ESP32S3 Sense
- 原 Arduino 固件：`omiGlass/firmware/`，迁移期间保持不动
- Opus：`78/esp-opus 1.0.5`
- Camera：`espressif/esp32-camera 2.1.8`
- 新增代码说明性注释统一使用中文

## 总体状态

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| Phase 0 | ESP-IDF 工程骨架 | 代码完成，待编译/真机验证 |
| Phase 1 | Board / GPIO / LED / Button | 代码完成，待硬件验证 |
| Phase 2 | PDM Mic | 代码完成，待硬件验证 |
| Phase 3 | Opus Encoder | 代码完成，待 v6.1 编译/真机验证 |
| Phase 4 | Native NimBLE / GATT | 代码完成，待手机 GATT 验证 |
| Phase 5 | BLE Audio TX | 代码完成，待手机实时音频验证 |
| Phase 6 | Camera | **代码完成，待 Camera 真机验证** |
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
- ESP32-S3 / 8MB Flash / Octal PSRAM。
- OTA 分区表迁移。
- `app_main()` 入口。
- `main/idf_component.yml` 固定 ESP-IDF `>=6.1,<6.2`。
- GitHub Actions 增加 `omiglass-esp-idf.yml`。

当前 GitHub 连接器仍未返回该 workflow 的实际运行状态，因此不能声明 CI 已通过。

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

保留：50ms 去抖、2 秒长按、启动闪灯和设备序列号语义。

---

## Phase 2：PDM 麦克风

```text
GPIO42 PDM CLK
GPIO41 PDM DATA
16kHz
Mono
PCM16
320 samples / 20ms
```

原 legacy `driver/i2s.h` 已迁移到 ESP-IDF v6.1 Channel API：

```text
i2s_new_channel()
i2s_channel_init_pdm_rx_mode()
i2s_channel_enable()
i2s_channel_read()
```

真机目标：约 `16000 samples/s`。

---

## Phase 3：Opus Encoder

参数保持：

```text
16kHz Mono
320 samples / 20ms
32kbps
Complexity 3
VBR ON
Codec ID 21
Max 160 bytes/frame
```

数据链：

```text
PCM -> 8000-sample ring buffer -> 320 samples -> opus_encode() -> encoded callback
```

详细说明：`PHASE3_OPUS.md`。

---

## Phase 4：Native NimBLE / GATT

已完成：

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

主要 GATT：

```text
Omi Main Service
  Audio Data     READ + NOTIFY
  Audio Codec    READ
  Photo Data     READ + NOTIFY
  Photo Control  WRITE
```

Preferred ATT MTU：517。

详细说明：`PHASE4_BLE_CORE.md`。

---

## Phase 5：BLE 实时音频发送

新增：

- `main/audio_tx_service.h/.cpp`
- `ble_service_notify_audio()`
- `PHASE5_BLE_AUDIO.md`

协议保持：

```text
byte 0-1  packet index，little-endian
byte 2    sub-index = 0
byte 3+   Opus payload
```

队列：16 帧。

实时策略：

- 未连接/未订阅不缓存旧语音。
- 队列满丢最旧帧。
- MTU 不足丢旧帧。
- notify 错误不持续重试旧帧。
- 每轮最多发送 2 帧。

当前链路：

```text
PDM Mic
  ↓
PCM16
  ↓
Opus
  ↓
audio_tx_service
  ↓
3-byte Omi Header
  ↓
ble_gatts_notify_custom()
  ↓
手机
```

理想真机状态：

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

## Phase 6：Camera

新增：

- `main/camera_service.h`
- `main/camera_service.cpp`
- `PHASE6_CAMERA.md`
- `espressif/esp32-camera 2.1.8` 依赖

Camera 参数保持原 Arduino 固件：

```text
VGA 640x480
JPEG
quality 25
XCLK 6MHz
1 framebuffer
PSRAM framebuffer
CAMERA_GRAB_LATEST
orientation = 2 / 180°
```

引脚：

```text
PWDN   -1
RESET  -1
XCLK   GPIO10
SIOD   GPIO40
SIOC   GPIO39
Y9     GPIO48
Y8     GPIO11
Y7     GPIO12
Y6     GPIO14
Y5     GPIO16
Y4     GPIO18
Y3     GPIO17
Y2     GPIO15
VSYNC  GPIO38
HREF   GPIO47
PCLK   GPIO13
```

ESP-IDF Camera 生命周期：

```text
esp_camera_init()
  ↓
esp_camera_fb_get()
  ↓
CameraFrameView
  ↓
esp_camera_fb_return()
```

当前 app_main 保持原固件默认意图：

```text
BLE 已连接
  ↓
每 30 秒拍一帧
```

但 Phase 6 只做：

```text
获取 JPEG
打印 size / width / height / orientation
立即归还 framebuffer
```

明确没有：

```text
Photo Data notify
图片分包
frame index
0xFFFF end marker
```

这些留给 Phase 7。

### 调度优先级

当前主循环顺序：

```text
Mic
 ↓
Opus
 ↓
BLE Audio TX
 ↓
Camera
```

继续保持：

```text
Audio > Photo
```

### Phase 6 真机目标

1. `esp_camera_init()` 成功。
2. `esp_camera_fb_get()` 稳定返回非空。
3. width / height = 640x480。
4. format = JPEG。
5. JPEG 长度合理且非零。
6. orientation = 2。
7. 连续抓图不泄漏 framebuffer。
8. 抓图时 BLE 不掉线。
9. 抓图时 Mic / Opus 不永久停止。
10. BLE Audio Queue 不持续堆积。

---

## 当前 app_main 链路

```text
app_main()
   │
   ├── board_service_init()
   ├── ble_service_init(serial)
   ├── audio_tx_service_init()
   ├── camera_service_init()
   ├── opus_service_init()
   ├── mic_service_start()
   │
   └── while(true)
          ├── BLE connected -> LED
          ├── board_service_process()
          ├── mic_service_process()
          ├── opus_service_process()
          ├── audio_tx_service_process()
          ├── Camera 30s capture/release
          ├── button event
          └── runtime statistics
```

---

## 当前验证边界

当前聊天模式可以直接读写 GitHub，但没有直接连接：

- 本地 ESP-IDF v6.1 编译器。
- XIAO ESP32S3 Sense 真机。
- 手机 BLE 调试环境。

因此当前准确状态是：

> **Phase 0～6 的迁移代码已经完成。**

尚不能声明：

> `idf.py build` 已通过、真机烧录通过、BLE 音频播放成功、Camera 真机拍照成功。

本地验证：

```bash
cd omiGlass/esp32
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

---

## 下一步：Phase 7 Photo BLE

Phase 7 会复用 `CameraFrameView`：

```text
JPEG framebuffer
  ↓
Photo BLE chunking
  ↓
frame index
  ↓
orientation byte
  ↓
Photo Data notify
  ↓
0xFFFF end marker
```

并继续执行：

```text
如果 Audio TX Queue 有数据
Photo BLE 主动让路
```

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
- Camera：VGA JPEG / quality 25 / XCLK 6MHz。
- 手机端 BLE 协议不变。
- Arduino `omiGlass/firmware/` 保持不动。
