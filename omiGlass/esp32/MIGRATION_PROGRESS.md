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

## 总体状态

- Phase 0：代码完成，待 ESP-IDF v6.1 编译 / 真机验证
- Phase 1：代码完成，待 GPIO / LED / Button 真机验证
- Phase 2：代码完成，待 PDM Mic 真机验证
- Phase 3：代码完成，待 Opus 编译 / 真机验证
- Phase 4：代码完成，待 Native NimBLE / 手机 GATT 验证
- Phase 5：代码完成，待 BLE Audio TX 真机验证
- Phase 6：代码完成，待 Camera 真机验证
- Phase 7：代码完成，待 Photo BLE 协议和 Audio+Photo 并发验证
- Phase 8：**代码迁移完成，待 ADC 校准、电压精度和 Battery BLE 真机验证**
- Phase 9：未开始
- Phase 10：未开始
- Phase 11：未开始
- Phase 12：未开始

---

## Phase 0：ESP-IDF 工程骨架

已完成：

- `omiGlass/esp32/` 原生 ESP-IDF 工程
- ESP32-S3 / 8MB Flash / Octal PSRAM
- OTA 分区表迁移
- `app_main()` 入口
- `main/idf_component.yml`
- GitHub Actions ESP-IDF v6.1 构建工作流

---

## Phase 1：板级基础能力

已迁移：

- GPIO1 电源按键
- GPIO21 状态 LED
- 50ms 去抖
- 2 秒长按识别
- `millis()` -> `esp_timer_get_time()`
- `delay()` -> `vTaskDelay()`
- `Serial.*` -> `ESP_LOGx`
- `ESP.getEfuseMac()` -> `esp_efuse_mac_get_default()`

保留：

- 按键低电平有效
- LED 反相：LOW=亮，HIGH=灭
- 启动闪灯序列
- 设备序列号兼容原 Arduino 生成方式

---

## Phase 2：PDM 麦克风

已迁移为 ESP-IDF v6.1 原生 PDM RX Channel API：

```text
GPIO42 CLK
GPIO41 DATA
16kHz / Mono / PCM16
320 samples / 20ms 每次读取
```

链路：

```text
PDM Mic
  ↓
i2s_channel_read()
  ↓
PCM callback
```

---

## Phase 3：Opus Encoder

参数保持：

- 16kHz
- Mono
- 320 samples / 20ms
- 32kbps
- Complexity 3
- VBR ON
- Codec ID 21
- 最大 160 bytes/frame

链路：

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

详细：`PHASE3_OPUS.md`

---

## Phase 4：Native NimBLE / GATT

已迁移：

- NimBLE Host 初始化
- Peripheral / Broadcaster / GATT Server
- GAP Device Name
- Advertising / Scan Response
- Connect / Disconnect
- MTU
- Connection Parameters
- CCCD / Subscribe Event
- Omi Main Service
- Battery Service
- Device Information Service
- OTA Service

Preferred ATT MTU：517。

详细：`PHASE4_BLE_CORE.md`

---

## Phase 5：BLE Audio TX

当前音频链：

```text
PDM Mic
  ↓
PCM
  ↓
Opus
  ↓
audio_tx_service
  ↓
3-byte Omi header
  ↓
Audio Data notify
```

协议：

```text
byte 0-1：packet index，little-endian
byte 2：sub-index = 0
byte 3...：Opus payload
```

策略：

- 16 帧实时队列
- 每轮最多发送 2 帧
- 队列满丢最旧帧
- 未连接 / 未订阅不积压旧语音
- MTU 不足时丢旧帧，实时性优先

详细：`PHASE5_BLE_AUDIO.md`

---

## Phase 6：Camera

已迁移：

- XIAO ESP32S3 Sense Camera GPIO
- VGA JPEG
- JPEG quality 25
- XCLK 6MHz
- 单 framebuffer
- PSRAM framebuffer
- `CAMERA_GRAB_LATEST`
- orientation = 2（180°）

Camera 生命周期：

```text
esp_camera_init()
  ↓
esp_camera_fb_get()
  ↓
CameraFrameView
  ↓
esp_camera_fb_return()
```

详细：`PHASE6_CAMERA.md`

---

## Phase 7：Photo BLE

新增：

- `main/photo_service.h`
- `main/photo_service.cpp`
- `PHASE7_PHOTO_BLE.md`

### Photo Data 协议

首包：

```text
byte 0   frame index low = 0
byte 1   frame index high = 0
byte 2   orientation
byte 3+  JPEG，最多 199 bytes
```

后续包：

```text
byte 0-1 frame index，little-endian
byte 2+  JPEG，最多 200 bytes
```

结束包：

```text
FF FF
```

最大 Photo Notification value：202 bytes。

最低可完整承载固定分包的 ATT MTU：205。

### Audio > Photo

主循环顺序：

```text
Mic
 ↓
Opus
 ↓
Audio TX
 ↓
Photo TX
```

Photo 内部再次检查：

```text
audio_tx_service_has_pending()
```

只要 Audio Queue 非空，本轮 Photo 立即让路。

Photo 每轮最多发送 2 个 chunk，与原 Arduino 固件一致。

### 完整性策略

Audio：

```text
notify 失败 -> 丢旧帧 -> 保实时
```

Photo：

```text
notify 失败 -> 不移动 offset/index -> 下轮重试同一 chunk
```

### framebuffer 生命周期

```text
Camera capture
   ↓
整个 JPEG 上传期间持续持有 framebuffer
   ↓
FF FF 成功发送
   ↓
release framebuffer
```

如果 BLE 断开或 Photo Data 取消订阅：

```text
取消上传
 ↓
立即 release framebuffer
```

### Photo Control

Characteristic：1 byte WRITE。

按原 `int8_t` 协议：

- `0xFF / -1`：单拍
- `0x00`：停止后续拍照
- `5..127`：启动周期拍照

周期仍固定使用原项目配置的 30 秒。

详细：`PHASE7_PHOTO_BLE.md`

---

## Phase 8：Battery ADC

新增：

- `main/battery_service.h`
- `main/battery_service.cpp`
- `PHASE8_BATTERY_ADC.md`

CMake 新增：

```text
esp_adc
```

### 原 Arduino 对应参数

```text
GPIO2 / A1
12-bit
ADC_11db
10 次采样平均
每次间隔 10ms
每 20 秒检查一次
```

硬件/算法参数：

```text
Voltage Divider Ratio   6.086
Battery Max             4.2V
Battery Min             3.2V
Critical                3.3V
Low                     3.4V
```

### ESP-IDF v6.1 ADC 链路

```text
GPIO2
 ↓
adc_oneshot_io_to_channel()
 ↓
ADC Unit / Channel
 ↓
adc_oneshot_read() × 10
 ↓
raw average
 ↓
Curve Fitting calibration
 ↓
adc_cali_raw_to_voltage()
 ↓
ADC mV
 ↓
× 6.086
 ↓
Battery Voltage
 ↓
Percentage + smoothing
 ↓
BLE Battery Level 0x2A19
```

ADC 配置：

```text
ADC_BITWIDTH_12
ADC_ATTEN_DB_12
```

不硬编码 ADC channel，由 ESP-IDF 根据 GPIO2 自动解析。

### 校准策略

正常路径：

```text
adc_cali_create_scheme_curve_fitting()
adc_cali_raw_to_voltage()
```

如果校准不可用，则退回原 Arduino 风格：

```text
raw / 4095 * 3300mV
```

### 百分比平滑

保留原逻辑：

```text
一次变化 <= 5% -> 直接采用
一次变化 > 5%  -> 只向目标方向移动 2%
```

### BLE Battery Service

标准服务保持：

```text
Battery Service   0x180F
Battery Level     0x2A19
READ + NOTIFY
```

Phase 4 的占位 0 已改为真实动态百分比。

BLE 层使用原子变量保存当前 Battery Level，避免 `app_main` 与 NimBLE Host Task 的跨任务数据竞争。

启动时：

```text
立即检测一次
 ↓
写入最新 Battery Level
```

运行时：

```text
每 20 秒检测
 ↓
更新 0x2A19
 ↓
已订阅时 Notify
```

手机刚订阅 Battery Notify 时，会立即尝试发送最近一次有效结果。

### Phase 8 不执行的策略

当前只记录：

```text
LOW       <= 3.4V
CRITICAL  <= 3.3V
```

不在本阶段：

- 降低 CPU 频率
- 关闭 Camera
- 关闭 Audio
- 自动 Deep Sleep
- 自动关机

这些留给 Phase 9。

详细：`PHASE8_BATTERY_ADC.md`

---

## 当前主链路

```text
app_main()
   │
   ├── board_service_init()
   ├── ble_service_init()
   ├── battery_service_init()
   ├── audio_tx_service_init()
   ├── camera_service_init()
   ├── photo_service_init()
   ├── opus_service_init()
   └── mic_service_start()
            │
            ↓
       while(true)
            │
            ├── Board / LED
            ├── Mic PCM
            ├── Opus Encode
            ├── BLE Audio TX
            ├── Photo Control / Capture / BLE TX
            ├── Battery ADC / BLE Battery Notify
            ├── Button Event
            └── Runtime Statistics
```

---

## 当前验证边界

当前聊天模式已经直接修改 GitHub 仓库，但尚未连接 XIAO ESP32S3 Sense 真机。

GitHub connector 当前也没有返回该分支 Actions run 的实际结果，因此暂时只声明：

> **Phase 0～8 代码迁移完成。**

尚不能声明：

- ESP-IDF v6.1 `idf.py build` 已通过
- 已成功烧录
- 手机 GATT 已验证
- Opus 音频已实际播放
- JPEG 已在手机端成功重组
- Audio + Photo 并发已稳定运行
- GPIO2 ADC 校准结果已与万用表核对
- Battery Level Notify 已真机验证

本地验证：

```bash
cd omiGlass/esp32
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

---

## 下一步：Phase 9

迁移 Power Management：

- CPU Frequency / PM Lock
- Idle / Power Save 状态
- Light Sleep
- Deep Sleep
- 长按关机
- GPIO1 唤醒
- Photo / Audio / BLE 活动对睡眠的约束
- Low Battery / Critical Battery 策略

---

## 固定兼容性约束

除非后续显式修改并记录：

- ESP-IDF 固定 v6.1.x
- XIAO ESP32S3 Sense
- BLE Device Name：`OMI Glass`
- Omi BLE UUID 不变
- Audio Codec ID：21
- 16kHz Mono
- Opus 320 samples / 20ms
- Audio BLE 包格式不变
- Camera：VGA JPEG
- Photo BLE 分包格式不变
- Battery Service 使用标准 `0x180F / 0x2A19`
- Arduino `omiGlass/firmware/` 保持不动
