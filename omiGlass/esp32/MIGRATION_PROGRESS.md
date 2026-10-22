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

| 阶段 | 内容 | 状态 |
|---|---|---|
| Phase 0 | ESP-IDF Skeleton | ✅ 代码完成 |
| Phase 1 | Board / GPIO / Button / LED | ✅ 代码完成 |
| Phase 2 | PDM Mic | ✅ 代码完成 |
| Phase 3 | Opus Encoder | ✅ 代码完成 |
| Phase 4 | Native NimBLE / GATT | ✅ 代码完成 |
| Phase 5 | BLE Audio TX | ✅ 代码完成 |
| Phase 6 | Camera | ✅ 代码完成 |
| Phase 7 | Photo BLE | ✅ 代码完成 |
| Phase 8 | Battery ADC | ✅ 代码完成 |
| Phase 9 | Power Management | ✅ **代码迁移完成** |
| Phase 10 | Wi-Fi / HTTPS OTA | ⬜ 未开始 |
| Phase 11 | 整机功能对齐 | ⬜ 未开始 |
| Phase 12 | 架构优化 | ⬜ 未开始 |

---

## 当前主链路

```text
app_main()
   │
   ├── board_service_init()
   ├── power_service_init()
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
            ├── Board / LED / Button
            ├── Mic PCM
            ├── Opus Encode
            ├── BLE Audio TX
            ├── Photo Control / Capture / BLE TX
            ├── Battery ADC / BLE Battery Notify
            ├── Power State / PM Lock / Sleep
            └── Runtime Statistics
```

---

## Phase 0～2

### Phase 0：ESP-IDF Skeleton

已完成：

- 原生 ESP-IDF 工程
- ESP32-S3 / 8MB Flash / Octal PSRAM
- OTA 分区表
- `app_main()`
- GitHub Actions v6.1 构建工作流

### Phase 1：Board

已迁移：

- GPIO1 电源按键
- GPIO21 LED
- 50ms 去抖
- 2 秒长按
- `millis()` -> `esp_timer_get_time()`
- `delay()` -> `vTaskDelay()`
- `Serial` -> `ESP_LOGx`
- eFuse MAC 序列号

### Phase 2：PDM Mic

```text
GPIO42 CLK
GPIO41 DATA
16kHz / Mono / PCM16
320 samples / 20ms
```

使用 ESP-IDF v6.1 新 I2S PDM RX Channel API。

---

## Phase 3～5：Audio / BLE

### Phase 3：Opus

```text
16kHz
Mono
320 samples / 20ms
32kbps
Complexity 3
VBR ON
Codec ID 21
```

详细：`PHASE3_OPUS.md`

### Phase 4：Native NimBLE

已迁移：

- Advertising
- Connect / Disconnect
- MTU 517
- CCCD
- Omi Main Service
- Battery Service
- Device Information
- OTA Service GATT 骨架

详细：`PHASE4_BLE_CORE.md`

### Phase 5：BLE Audio TX

```text
Opus Frame
 ↓
16-frame realtime queue
 ↓
2-byte packet index
+ 1-byte sub-index
 ↓
Audio Data Notify
```

策略：实时性优先，队列满时丢最旧音频。

详细：`PHASE5_BLE_AUDIO.md`

---

## Phase 6～7：Camera / Photo BLE

### Phase 6：Camera

保持：

```text
VGA 640x480
JPEG quality 25
XCLK 6MHz
1 framebuffer
PSRAM
orientation = 180°
```

详细：`PHASE6_CAMERA.md`

### Phase 7：Photo BLE

协议保持：

```text
首包：index(2) + orientation(1) + JPEG <=199 bytes
后续：index(2) + JPEG <=200 bytes
结束：FF FF
```

调度原则：

```text
Audio > Photo
```

图片 notify 失败时不移动 offset，下轮重试同一 chunk，保证 JPEG 完整性。

详细：`PHASE7_PHOTO_BLE.md`

---

## Phase 8：Battery ADC

已完成：

```text
GPIO2
ADC oneshot
ADC_BITWIDTH_12
ADC_ATTEN_DB_12
10次采样平均
Curve Fitting calibration
分压比 6.086
20秒检测周期
```

BLE：

```text
Battery Service 0x180F
Battery Level   0x2A19
READ + NOTIFY
```

阈值：

```text
LOW      <= 3.4V
CRITICAL <= 3.3V
```

详细：`PHASE8_BATTERY_ADC.md`

---

## Phase 9：Power Management

新增：

```text
main/power_service.h
main/power_service.cpp
PHASE9_POWER_MANAGEMENT.md
```

### ESP-IDF v6.1 PM

```text
Max CPU = 80MHz
Min CPU = 40MHz
DFS = ON
Tickless Idle = ON
Automatic Light Sleep = ON
Bluetooth Modem Sleep = ON
```

使用 PM Lock：

```text
ESP_PM_CPU_FREQ_MAX
ESP_PM_NO_LIGHT_SLEEP
```

### 状态

```text
ACTIVE
POWER_SAVE
LOW_BATTERY
POWERING_OFF
```

### 规则

```text
BLE断开 + 空闲 >=45秒
 -> POWER_SAVE / 允许40MHz

Battery <=3.4V
 -> LOW_BATTERY

长按2秒
Battery <=3.3V
BLE断开且空闲 >=5分钟
 -> POWERING_OFF -> Deep Sleep
```

### Light Sleep

不再照搬旧 Arduino 的手工：

```cpp
esp_light_sleep_start();
```

原因：手工 Light Sleep 无法维持 BLE。

改为 ESP-IDF 官方模式：

```text
Bluetooth Modem Sleep
+
FreeRTOS Tickless Idle
+
Automatic Light Sleep
```

Audio / Photo 活动时持有 No-Light-Sleep Lock。

### Deep Sleep

进入前依次：

```text
清 Audio Queue
取消 Photo / 释放 framebuffer
停止 Mic / I2S
释放 Opus
Camera deinit
Battery ADC deinit
NimBLE stop / deinit
LED OFF
Deep Sleep
```

唤醒：

```text
GPIO1 LOW
```

使用 ESP-IDF v6.1：

```cpp
esp_sleep_enable_ext1_wakeup_io(..., ESP_EXT1_WAKEUP_ANY_LOW);
```

长按关机时会等待用户先松开 GPIO1，再真正进入 Deep Sleep，避免“刚睡就立即被仍按住的按钮唤醒”。

详细：`PHASE9_POWER_MANAGEMENT.md`

---

## 当前验证边界

目前聊天模式已经直接把 Phase 0～9 的代码写入 GitHub，但没有连接真实 XIAO ESP32S3 Sense。

GitHub connector 当前也没有返回该分支的 Actions run，因此只声明：

> **Phase 0～9 代码迁移完成。**

尚未声明：

- ESP-IDF v6.1 `idf.py build` 已通过
- 固件已成功烧录
- BLE GATT / Audio / Photo 已手机实测
- ADC 已和万用表核对
- 80/40MHz DFS 已真机验证
- Automatic Light Sleep 电流已测量
- GPIO1 Deep Sleep 唤醒已验证
- Low / Critical Battery 策略已真机验证

本地验证：

```bash
cd omiGlass/esp32
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

---

## 下一步：Phase 10

迁移 OTA：

```text
BLE OTA Control
 ↓
Wi-Fi credentials / URL
 ↓
esp_wifi
 ↓
esp_http_client / esp_https_ota
 ↓
esp_ota_ops
 ↓
OTA partition
 ↓
reboot
```

重点：

- 保持现有 OTA BLE command 协议
- 去掉 Arduino `WiFi.h / HTTPClient.h / Update.h`
- 不再使用 `setInsecure()`
- 使用 ESP-IDF 证书校验 / cert bundle
- OTA 期间通过 PM Lock 保持性能并禁止 Light Sleep

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
- Battery Service：`0x180F / 0x2A19`
- Arduino `omiGlass/firmware/` 保持不动
