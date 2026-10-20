# OmiGlass ESP-IDF 迁移进度

分支：`feature/omiglass-esp-idf-migration`

最后更新：2026-10-20

## 总体状态

- Phase 0：代码完成，待本地 `idf.py build` / 真机启动验证
- Phase 1：代码迁移完成，待本地编译与硬件验证
- Phase 2：未开始
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
- 麦克风 GPIO42 / GPIO41 常量先迁入共享配置，功能留待 Phase 2。
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
- 当前 `app_main()` 以 10ms 周期运行板级基础处理。

### 4. 设备序列号

原 Arduino 代码通过：

```cpp
uint64_t chipId = ESP.getEfuseMac();
snprintf(serialNumber, sizeof(serialNumber), "%04X%08X", ...);
```

ESP-IDF 版本使用 `esp_efuse_mac_get_default()`，并刻意保留 Arduino `ESP.getEfuseMac()` 将 6 字节 MAC 直接写入 `uint64_t` 的数值布局，以尽量保证迁移前后设备序列号一致。

注意：最终仍需在同一块 XIAO ESP32S3 上分别运行 Arduino 与 ESP-IDF 固件，比较实际输出的序列号后才能把“完全兼容”标记为已验证。

### 5. app_main 入口

当前 ESP-IDF `app_main()` 已完成：

- 打印固件版本和硬件版本。
- 打印芯片核心数和修订版本。
- 初始化板级基础服务。
- 打印设备序列号。
- 处理 LED 状态。
- 处理按键短按/长按事件。

## Phase 1 暂未接入的行为

以下行为是有意延后，而不是遗漏：

- 长按后的真正关机 / Deep Sleep：Phase 9 电源管理阶段。
- CPU 动态频率调整：Phase 9。
- BLE 连接后 LED 常亮：Phase 4 接入 BLE 状态后启用。
- 短按后的业务状态变化：等主业务状态机逐步迁移后接入。

## 当前验证状态

### 已完成的静态确认

- 原 Arduino 固件仍未修改。
- ESP-IDF 代码不依赖 `Arduino.h`。
- Phase 1 不依赖 NimBLE-Arduino。
- GPIO、按键有效电平、LED 反相逻辑均按原固件迁移。
- 设备序列号算法按 Arduino Core 的 `ESP.getEfuseMac()` 实现方式做兼容迁移。

### 尚未执行

当前聊天模式只通过 GitHub 连接器读写仓库，没有连接本地 ESP-IDF 编译器和 XIAO ESP32S3 真机，因此以下项目尚未声称完成：

```bash
cd omiGlass/esp32
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

真机需要验证：

1. 能成功编译。
2. 能成功烧录和启动。
3. GPIO21 LED 启动闪烁正常。
4. GPIO1 短按识别正常。
5. GPIO1 按住 2 秒能产生长按事件。
6. ESP-IDF 与 Arduino 版本输出的设备序列号完全一致。

## 下一步

进入 Phase 2：迁移 XIAO ESP32S3 Sense PDM 麦克风。

计划采用 ESP-IDF 原生 PDM RX I2S 驱动：

- GPIO42：PDM CLK。
- GPIO41：PDM DATA。
- 16kHz。
- 单声道 PCM16。
- 保留原固件的麦克风回调语义，方便 Phase 3 直接接 Opus Encoder。

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
