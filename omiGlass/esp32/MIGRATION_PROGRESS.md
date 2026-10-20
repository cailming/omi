# OmiGlass ESP-IDF 迁移进度

分支：`feature/omiglass-esp-idf-migration`

最后更新：2026-10-20

## 当前 ESP-IDF 基线

当前新增代码按 ESP-IDF v6.1 的原生驱动接口编写，尤其是拆分后的 `esp_driver_gpio` / `esp_driver_i2s` 组件和新版 I2S Channel API。

如果后续决定固定到其他版本（例如 v5.5 LTS 风格分支），需要再做一次 CMake 依赖与驱动 API 兼容检查。

## 总体状态

- Phase 0：代码完成，待本地 `idf.py build` / 真机启动验证
- Phase 1：代码迁移完成，待本地编译与硬件验证
- Phase 2：代码迁移完成，待本地编译与麦克风真机验证
- Phase 3：未开始
- Phase 4：未开始
- Phase 5：未开始
- Phase 6：未开始
- Phase 7：未开始
- Phase 8：未开始
- Phase 9：未开始
- Phase 10：未开始
- Phase 11：未开始
- Phase 12：未开始

## Phase 0 已完成

- 创建独立迁移分支 `feature/omiglass-esp-idf-migration`。
- 创建 `omiGlass/esp32/` 原生 ESP-IDF 工作区。
- 保留 `omiGlass/firmware/` Arduino/PlatformIO 固件作为功能参考，不修改原实现。
- 创建 ESP-IDF 根 `CMakeLists.txt`。
- 创建 `main/CMakeLists.txt` 与 `app_main.cpp`。
- 添加 ESP32-S3 / 8MB Flash / PSRAM 初始配置。
- OTA 分区表按原 Arduino 固件的布局迁移。
- 创建迁移计划和本进度文件。

## Phase 1 已完成的代码迁移

### 1. 配置与硬件映射

新增 `main/omiglass_config.h`：

- 设备名称、固件版本、硬件版本、厂商信息。
- 电源按键：GPIO1。
- 状态 LED：GPIO21。
- 保留原固件的 LED 反相逻辑：LOW=亮，HIGH=灭。
- Battery GPIO2 常量先迁入共享配置，ADC 功能留待后续阶段。
- Camera GPIO 映射迁入共享配置，初始化留待 Camera 阶段。

### 2. 板级基础服务

新增 `main/board_service.h/.cpp`：

- `pinMode()` -> `gpio_config()`。
- `digitalRead()` -> `gpio_get_level()`。
- `digitalWrite()` -> `gpio_set_level()`。
- `attachInterrupt(..., CHANGE)` -> ESP-IDF GPIO `GPIO_INTR_ANYEDGE` + ISR service。
- 按键 ISR 只记录事件，去抖和长按识别放在普通任务上下文执行。
- 保留 50ms 按键去抖。
- 保留 2 秒长按判断。
- 保留启动约 1.5 秒快速闪灯逻辑。
- BLE 尚未迁移时，启动序列结束后按“未连接”状态慢闪。
- 为后续 BLE 模块预留 `board_service_set_connected()`。

### 3. Arduino 时间 API 替换

- `millis()` -> `esp_timer_get_time() / 1000`。
- `delay()` -> `vTaskDelay(pdMS_TO_TICKS(...))`。

### 4. 设备序列号

原 Arduino 代码通过：

```cpp
uint64_t chipId = ESP.getEfuseMac();
snprintf(serialNumber, sizeof(serialNumber), "%04X%08X", ...);
```

ESP-IDF 版本使用 `esp_efuse_mac_get_default()`，并刻意保留 Arduino `ESP.getEfuseMac()` 将 6 字节 MAC 直接写入 `uint64_t` 的数值布局，以尽量保证迁移前后设备序列号一致。

注意：最终仍需在同一块 XIAO ESP32S3 上分别运行 Arduino 与 ESP-IDF 固件，比较实际输出的序列号后才能把“完全兼容”标记为已验证。

## Phase 2 已完成的代码迁移

新增：

- `main/mic_service.h`
- `main/mic_service.cpp`

### 原 Arduino 实现

原代码使用旧版 I2S 驱动：

```cpp
#include <driver/i2s.h>
I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM
i2s_driver_install(...)
i2s_set_pin(...)
i2s_read(...)
```

### ESP-IDF 原生实现

已迁移为新 I2S Channel API：

```cpp
#include "driver/i2s_pdm.h"
i2s_new_channel(...)
i2s_channel_init_pdm_rx_mode(...)
i2s_channel_enable(...)
i2s_channel_read(...)
```

具体参数：

- I2S：I2S0。
- 模式：PDM RX Master。
- PDM CLK：GPIO42。
- PDM DATA：GPIO41。
- 输出格式：PCM16。
- 声道：Mono。
- Sample Rate：16kHz。
- Gain：2，保留原固件的软件增益和 16-bit 饱和处理。
- DMA descriptors：8。
- DMA frame num：256。
- 总读取缓冲区：1600 samples。
- 单次读取：320 samples / 20ms。
- 单次读超时：30ms。

### 为什么改成 320 samples 一次读取

原 Arduino 配置定义了 1600 samples 的读取缓冲区，但 `i2s_read()` 的行为与新版 `i2s_channel_read()` 不完全相同。

新版 API 会等待请求的数据块完成或达到超时，因此 ESP-IDF 版本把实际单次请求调整为：

```text
16000 samples/s × 20ms = 320 samples
```

这样有三个好处：

1. 更适合实时音频，减少一次读取造成的额外延迟。
2. 正好等于原 Opus 配置的一帧 `OPUS_FRAME_SAMPLES = 320`。
3. Phase 3 可以直接把每次 PCM 回调送入 Opus，而不必重新切帧。

这属于底层调度优化，不改变手机端协议。

### 内存策略

- 优先使用 `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` 分配麦克风缓冲区。
- PSRAM 不可用时回退普通 RAM。
- 停止麦克风时释放 I2S Channel 和缓冲区。

### 当前 app_main 中的 Phase 2 验证逻辑

当前阶段还没有接 Opus。

PCM 回调只做轻量统计：

- 每秒累计收到的 sample 数。
- 计算最近一秒 PCM 峰值。

正常情况下，真机日志应接近：

```text
麦克风采集状态：最近约 1 秒收到 16000 samples，峰值=...
```

Phase 3 接入 Opus 后，这个临时统计回调将替换为 Opus PCM 输入。

## Phase 1 / Phase 2 暂未接入的行为

以下行为是有意延后，而不是遗漏：

- 长按后的真正关机 / Deep Sleep：Phase 9。
- CPU 动态频率调整：Phase 9。
- BLE 连接后 LED 常亮：Phase 4。
- 短按后的业务状态变化：主业务状态机逐步迁移时接入。
- Opus 编码：Phase 3。
- BLE 音频上传：Phase 5。

## 当前验证状态

### 已完成的静态确认

- 原 Arduino 固件仍未修改。
- ESP-IDF 新代码不依赖 `Arduino.h`。
- 不依赖 NimBLE-Arduino。
- Phase 2 不再使用 legacy `driver/i2s.h`。
- GPIO、按键有效电平、LED 反相逻辑按原固件迁移。
- 麦克风 GPIO、采样率、Mono/PCM16、Gain 按原固件迁移。
- 麦克风数据回调模式被保留，便于后续直接接 Opus。

### 尚未执行

当前聊天模式只通过 GitHub 连接器读写仓库，没有连接本地 ESP-IDF 编译器和 XIAO ESP32S3 真机，因此以下项目尚未声称完成：

```bash
cd omiGlass/esp32
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

真机需要验证：

1. ESP-IDF v6.1 环境下成功编译。
2. 能成功烧录和启动。
3. GPIO21 LED 启动闪烁正常。
4. GPIO1 短按识别正常。
5. GPIO1 按住 2 秒能产生长按事件。
6. ESP-IDF 与 Arduino 版本输出的设备序列号完全一致。
7. PDM CLK GPIO42 有稳定时钟。
8. 麦克风每秒收到约 16000 samples。
9. 静音和说话时 PCM 峰值有明显变化。
10. 长时间采集没有持续 timeout / overrun / heap 异常。

## 下一步

进入 Phase 3：迁移 Opus Encoder。

目标继续保持：

- 16kHz。
- Mono。
- 320 samples / 20ms。
- 32kbps。
- Complexity 3。
- VBR 开启。
- Codec ID 21。
- 编码输出最大 160 bytes。

优先方案：使用可被 ESP-IDF Component Manager 管理的 Opus 组件，而不是继续依赖 Arduino `arduino-libopus` 封装。

## 迁移期间固定不变的兼容性约束

除非后续显式修改并记录，否则以下内容保持不变：

- 硬件：Seeed XIAO ESP32S3 Sense。
- BLE 设备名：`OMI Glass`。
- Omi BLE UUID。
- Audio Codec ID：21（Opus）。
- 音频采样率：16kHz。
- Opus Frame：320 samples / 20ms。
- Camera 基线：VGA JPEG。
- 手机端 BLE 数据包格式不变。
- Arduino 固件在迁移期间保持原样。
