# OmiGlass ESP-IDF 迁移进度

分支：`feature/omiglass-esp-idf-migration`

最后更新：2026-10-21

## 固定开发基线

- ESP-IDF：**v6.1.x**
- 版本约束：`>=6.1,<6.2`
- Target：ESP32-S3
- 硬件：Seeed Studio XIAO ESP32S3 Sense
- 原 Arduino 固件：`omiGlass/firmware/`，迁移期间保持不动
- 当前 Opus 组件：`78/esp-opus 1.0.5`

## 总体状态

- Phase 0：代码完成，待本地 `idf.py build` / 真机验证
- Phase 1：代码完成，待本地编译与 GPIO/LED/Button 验证
- Phase 2：代码完成，待 PDM Mic 真机验证
- Phase 3：代码完成，待 ESP-IDF v6.1 + Opus 编译/真机验证
- Phase 4：**代码迁移完成，待 ESP-IDF v6.1 编译与手机 GATT 验证**
- Phase 5：未开始
- Phase 6：未开始
- Phase 7：未开始
- Phase 8：未开始
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
- 迁移计划与进度文件

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
- 设备序列号尽量兼容 Arduino 版本

---

## Phase 2：PDM 麦克风

已迁移为 ESP-IDF v6.1 原生 PDM RX Channel API：

```text
GPIO42 CLK
GPIO41 DATA
16kHz / Mono / PCM16
320 samples / 20ms 每次读取
```

数据链：

```text
PDM Mic
  ↓
i2s_channel_read()
  ↓
PCM callback
```

真机目标：约 `16000 samples/s`。

---

## Phase 3：Opus Encoder

已完成：

- `main/idf_component.yml`
- `main/opus_service.h/.cpp`
- `PHASE3_OPUS.md`

参数保持：

- 16kHz
- Mono
- 320 samples / 20ms
- 32kbps
- Complexity 3
- VBR ON
- Codec ID 21
- 最大 160 bytes/frame

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

Phase 3 仍然不发送 BLE。

---

## Phase 4：Native NimBLE / GATT

新增：

- `main/ble_service.h`
- `main/ble_service.cpp`
- `PHASE4_BLE.md`

CMake 已加入：

- `bt`
- `nvs_flash`

`sdkconfig.defaults` 已启用：

- NimBLE
- Peripheral role
- Broadcaster role
- GATT Server
- GAP Service
- 最大连接数 1
- Preferred ATT MTU 517

### 已迁移的 BLE Core

- `nimble_port_init()`
- NimBLE Host FreeRTOS Task
- GAP Device Name
- Advertising
- Scan Response
- Connect / Disconnect
- MTU Event
- Connection Parameters
- CCCD / Subscribe Event
- GATT Server

### GATT 结构

#### Omi Main Service

`19B10000-E8F2-537E-4F6C-D104768A1214`

- Audio Data：Read + Notify
- Audio Codec：Read，返回 Codec ID 21
- Photo Data：Read + Notify
- Photo Control：Write

#### Battery Service

- `0x180F`
- Battery Level `0x2A19`：Read + Notify
- Phase 8 前返回占位值 0

#### Device Information Service

- `0x180A`
- Manufacturer Name
- Model Number
- Firmware Revision
- Hardware Revision
- Serial Number

#### OTA Service

`19B10010-E8F2-537E-4F6C-D104768A1214`

- OTA Control：Read + Write
- OTA Data：Read + Notify
- Phase 10 前只保留协议入口，不执行真实 OTA

### 广播

保持原 Arduino 设计意图：

```text
Advertising Data:
  Omi Main Service 128-bit UUID

Scan Response:
  OMI Glass
```

广播间隔：

- 200ms minimum
- 400ms maximum

### 连接参数

- Preferred ATT MTU：517
- Connection Interval：25~50ms
- Latency：0
- Supervision Timeout：8s

### Phase 4 明确不做的事情

`on_opus_encoded()` 当前仍只做统计，不会进入 BLE notify。

因此当前链路是：

```text
Mic
 ↓
Opus
 ↓
on_opus_encoded()
 ↓
统计

× 不发送 Audio Data Notify
```

Phase 5 才改成：

```text
Opus Frame
 ↓
Audio TX Ring Buffer
 ↓
3-byte header
 ↓
Audio Data Notify
```

### Phase 4 真机验证目标

1. 手机扫描到 `OMI Glass`
2. 能成功连接
3. GATT 服务/Characteristic 与 Arduino 版本一致
4. Audio Codec 读取 `0x15`
5. Audio Data / Photo Data / Battery / OTA Data 可订阅
6. Photo Control 写命令可在串口看到
7. OTA Control 写命令可在串口看到，但不会执行 OTA
8. Device Information 可读取
9. ATT MTU 协商正常
10. 断开后自动恢复广播
11. BLE 连接状态能驱动 LED 状态
12. Mic / Opus 保持运行，但无实时 Audio notify 流量

详细内容见：`PHASE4_BLE.md`

---

## 当前 app_main 链路

```text
app_main()
   │
   ├── board_service_init()
   │
   ├── ble_service_init()
   │      └── NimBLE Host Task
   │
   ├── opus_service_init()
   │
   ├── mic_service_start()
   │
   └── while(true)
          ├── BLE connected -> LED
          ├── board_service_process()
          ├── mic_service_process()
          ├── opus_service_process()
          ├── button event
          └── audio statistics
```

---

## 当前验证边界

当前聊天模式能够通过 GitHub 直接读写仓库，但没有连接实际 ESP-IDF v6.1 编译环境或 XIAO ESP32S3 真机。

因此目前只声明：

> **Phase 0~4 代码迁移已完成。**

尚不能声明：

> 已通过 `idf.py build`、烧录、手机连接或真机稳定性测试。

本地验证命令：

```bash
cd omiGlass/esp32
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

---

## 下一步：Phase 5

Phase 5 迁移 BLE Audio TX：

- Audio TX ring buffer
- packet index
- sub-index
- 3-byte packet header
- Audio Data notify
- BLE congestion / pacing
- 音频优先级

手机端包格式保持：

```text
byte 0-1：packet index，小端
byte 2：sub-index
byte 3...：Opus payload
```

---

## 固定兼容性约束

除非后续显式修改并记录：

- ESP-IDF 固定 v6.1.x
- XIAO ESP32S3 Sense
- BLE Device Name：`OMI Glass`
- Omi BLE UUID 不变
- Codec ID：21
- 16kHz Mono
- Opus 320 samples / 20ms
- Camera：VGA JPEG
- 手机端 BLE 包格式不变
- Arduino `omiGlass/firmware/` 保持不动
