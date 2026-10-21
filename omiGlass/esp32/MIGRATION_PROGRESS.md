# OmiGlass ESP-IDF 迁移进度

分支：`feature/omiglass-esp-idf-migration`

最后更新：2026-10-21

## 固定开发基线

- ESP-IDF：**v6.1.x**
- 版本约束：`>=6.1,<6.2`
- Target：ESP32-S3
- 硬件：Seeed Studio XIAO ESP32S3 Sense
- 原 Arduino 固件：`omiGlass/firmware/`，迁移期间保持不动

ESP-IDF 版本约束和第三方组件统一写入：

`main/idf_component.yml`

当前 Opus 依赖：

`78/esp-opus 1.0.5`

## 总体状态

- Phase 0：代码完成，待本地 `idf.py build` / 真机启动验证
- Phase 1：代码迁移完成，待本地编译与硬件验证
- Phase 2：代码迁移完成，待本地编译与麦克风真机验证
- Phase 3：**代码迁移完成，待 ESP-IDF v6.1 编译与 Opus 真机验证**
- Phase 4：未开始
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

- 创建独立迁移分支。
- 创建 `omiGlass/esp32/` 原生 ESP-IDF 工作区。
- 创建 ESP-IDF 根 `CMakeLists.txt`。
- 创建 `main/CMakeLists.txt` 与 `app_main.cpp`。
- 配置 ESP32-S3 / 8MB Flash / Octal PSRAM。
- OTA 分区表按原 Arduino 固件迁移。
- 创建迁移计划与进度记录。

---

## Phase 1：板级基础能力

新增：

- `main/omiglass_config.h`
- `main/board_service.h`
- `main/board_service.cpp`

已迁移：

- `pinMode()` -> `gpio_config()`
- `digitalRead()` -> `gpio_get_level()`
- `digitalWrite()` -> `gpio_set_level()`
- `attachInterrupt(..., CHANGE)` -> GPIO ISR Service + `GPIO_INTR_ANYEDGE`
- `millis()` -> `esp_timer_get_time()`
- `delay()` -> `vTaskDelay()`
- `ESP.getEfuseMac()` -> `esp_efuse_mac_get_default()`
- `Serial.*` -> `ESP_LOGx`

硬件行为保持：

- GPIO1：电源按键
- 内部上拉
- 低电平按下
- GPIO21：状态 LED
- LED 反相逻辑：LOW=亮，HIGH=灭
- 50ms 去抖
- 2 秒长按
- 启动约 1.5 秒快速闪灯

设备序列号刻意兼容原 Arduino `ESP.getEfuseMac()` 的数值布局，最终仍需同板 A/B 对比验证。

暂缓：

- 长按后的 Deep Sleep：Phase 9
- CPU 动态频率：Phase 9
- BLE 连接后的 LED 状态：Phase 4

---

## Phase 2：PDM 麦克风

新增：

- `main/mic_service.h`
- `main/mic_service.cpp`

原 Arduino 使用 legacy I2S：

```cpp
#include <driver/i2s.h>
i2s_driver_install(...)
i2s_set_pin(...)
i2s_read(...)
```

已改为 ESP-IDF v6.1 原生 PDM RX Channel API：

```cpp
#include "driver/i2s_pdm.h"
i2s_new_channel(...)
i2s_channel_init_pdm_rx_mode(...)
i2s_channel_enable(...)
i2s_channel_read(...)
```

当前参数：

- I2S0
- PDM RX Master
- CLK GPIO42
- DATA GPIO41
- PCM16 Mono
- 16000 Hz
- 软件 Gain 2
- DMA descriptors 8
- DMA frame num 256
- 总读取缓冲 1600 samples
- 单次读取 320 samples / 20ms
- 读取超时 30ms

内存：优先 PSRAM，失败后回退普通 RAM。

Phase 2 真机目标：

```text
麦克风：最近约 1 秒收到 16000 samples，峰值=...
```

---

## Phase 3：Opus Encoder

新增：

- `main/idf_component.yml`
- `main/opus_service.h`
- `main/opus_service.cpp`
- `PHASE3_OPUS.md`

### 构建依赖

固定：

```yaml
dependencies:
  idf:
    version: ">=6.1,<6.2"
  78/esp-opus:
    version: "1.0.5"
```

不再继续依赖原 PlatformIO `arduino-libopus` 封装。

`dependencies.lock` 不手工写入，等待 ESP-IDF v6.1 首次实际执行依赖解析后自动生成。

### 编码参数

保持原 Arduino 固件：

- 16kHz
- Mono
- `OPUS_APPLICATION_VOIP`
- 320 samples / 20ms
- 32kbps
- Complexity 3
- Voice Signal
- VBR 开启
- VBR Constraint 关闭
- LSB Depth 16
- DTX 关闭
- In-band FEC 关闭
- Packet Loss Percentage 0
- 输出最大 160 bytes
- Omi Codec ID 21

### 数据链路

```text
PDM Mic
   ↓
PCM16 / 16kHz / mono
   ↓
mic_service callback
   ↓
opus_service_receive_pcm()
   ↓
8000 samples PCM ring buffer
   ↓
每满 320 samples
   ↓
opus_encode()
   ↓
encoded callback
```

虽然当前麦克风通常一次读取 320 samples，但 Opus 服务仍支持任意长度 PCM 输入，避免 I2S 与编码器强耦合。

### 环形缓冲行为

保留原 Arduino 策略：

- 容量：8000 samples，约 500ms
- 缓冲满时不阻塞采集
- 丢弃最旧 sample
- 实时音频链路优先

### 当前验证日志

BLE 尚未迁移，因此 Opus 帧暂不发送，只统计：

- 每秒编码帧数
- 每秒编码总字节数
- 平均帧长
- 最大帧长

理论目标：

```text
16000 / 320 = 50 frames/s
```

预期日志：

```text
麦克风：最近约 1 秒收到 16000 samples，峰值=...
Opus：最近约 1 秒编码 50 帧，共 ... 字节，平均 ... 字节/帧，最大 ... 字节
```

VBR 下帧长度会变化，但当前协议要求单帧不得超过 160 bytes。

---

## 当前 app_main 链路

```text
app_main()
   │
   ├── board_service_init()
   │
   ├── opus_service_init()
   │
   ├── mic_service_start()
   │
   └── while(true)
          │
          ├── board_service_process()
          ├── mic_service_process()
          │      ↓ PCM
          ├── opus_service_receive_pcm()
          ├── opus_service_process()
          │      ↓ Opus frame
          ├── on_opus_encoded()
          ├── button event
          └── audio statistics
```

---

## 当前验证状态

### 已完成的静态确认

- 原 Arduino `omiGlass/firmware/` 未修改。
- ESP-IDF 新代码不依赖 `Arduino.h`。
- 不依赖 NimBLE-Arduino。
- 麦克风不使用 legacy `driver/i2s.h`。
- GPIO、按键、LED 行为按原固件迁移。
- 麦克风硬件参数按原固件迁移。
- Opus 编码参数按原固件迁移。
- Omi Codec ID 保持 21。
- Opus 与 Mic 之间通过 PCM 环形缓冲解耦。

### 尚未执行

当前聊天模式没有连接本地 ESP-IDF 编译环境和实体 XIAO ESP32S3，因此以下项目尚未声称完成：

```bash
cd omiGlass/esp32
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

真机/本地需要验证：

1. ESP-IDF v6.1.x 正确识别版本约束。
2. Component Manager 成功解析 `78/esp-opus 1.0.5`。
3. `idf.py build` 完成。
4. LED / Button 正常。
5. 设备序列号与 Arduino 版本一致。
6. 麦克风约 16000 samples/s。
7. 说话时 PCM 峰值明显变化。
8. Opus 约 50 frames/s。
9. Opus 单帧 <= 160 bytes。
10. 连续运行无明显 heap 泄漏、I2S timeout 风暴、Opus encode 错误。

---

## 下一步

进入 **Phase 4：BLE Core / GATT Services**。

重点迁移：

- NimBLE-Arduino -> ESP-IDF Native NimBLE
- Omi Main Service
- Audio Data Characteristic
- Audio Codec Characteristic
- Photo Data Characteristic
- Photo Control Characteristic
- Battery Service
- Device Information Service
- OTA Service
- CCCD subscription
- Connect / Disconnect
- Advertising
- MTU / connection parameters

Phase 4 先建立 BLE 服务结构和连接状态，不立即发送实时 Opus。

Phase 5 再把 `on_opus_encoded()` 接到 BLE Audio TX 队列，保持原音频包格式：

```text
byte 0-1：packet index，小端
byte 2：sub-index
byte 3...：Opus payload
```

---

## 迁移期间固定不变的兼容性约束

除非后续显式修改并记录，否则：

- ESP-IDF 固定 v6.1.x
- 硬件固定 Seeed XIAO ESP32S3 Sense
- BLE Device Name：`OMI Glass`
- Omi BLE UUID 不变
- Audio Codec ID：21
- Audio Sample Rate：16kHz
- Opus Frame：320 samples / 20ms
- Camera 基线：VGA JPEG
- 手机端 BLE 数据包格式不变
- Arduino 固件保持原样
