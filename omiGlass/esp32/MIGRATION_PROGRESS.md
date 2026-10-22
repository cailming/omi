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
| Phase 0 | ESP-IDF Skeleton | ✅ 已迁移 / CI 编译通过 |
| Phase 1 | Board / GPIO / Button / LED | ✅ 已迁移 / CI 编译通过 |
| Phase 2 | PDM Mic | ✅ 已迁移 / CI 编译通过 |
| Phase 3 | Opus Encoder | ✅ 已迁移 / CI 编译通过 |
| Phase 4 | Native NimBLE / GATT | ✅ 已迁移 / CI 编译通过 |
| Phase 5 | BLE Audio TX | ✅ 已迁移 / CI 编译通过 |
| Phase 6 | Camera | ✅ 已迁移 / CI 编译通过 |
| Phase 7 | Photo BLE | ✅ 已迁移 / CI 编译通过 |
| Phase 8 | Battery ADC | ✅ 已迁移 / CI 编译通过 |
| Phase 9 | Power Management | ✅ 已迁移 / CI 编译通过 |
| Phase 10 | Wi-Fi / HTTPS OTA | ✅ **已迁移 / ESP-IDF v6.1 CI 编译通过** |
| Phase 11 | 整机功能对齐 | ✅ **软件侧可迁移/可静态验证部分已收口；🚧 仅剩真机验证** |
| Phase 12 | 架构优化 | ⬜ 未开始 |

> “CI 编译通过”只表示源码已经在官方 ESP-IDF v6.1 环境中成功构建，不等于真机硬件和手机端联调已经通过。

---

## 当前主链路

```text
app_main()
   │
   ├── board_service
   ├── power_service
   ├── ota_service
   ├── ble_service
   ├── battery_service
   ├── audio_tx_service
   ├── camera_service
   ├── photo_service
   ├── opus_service
   └── mic_service
```

运行时主要链路：

```text
PDM Mic
   ↓
PCM16 / 16kHz
   ↓
Opus 20ms
   ↓
BLE Audio
```

```text
Camera VGA JPEG
   ↓
Photo Chunk
   ↓
BLE Photo
```

```text
GPIO2 ADC
   ↓
Calibration
   ↓
Battery %
   ↓
BLE Battery Service
```

```text
BLE OTA Control
   ↓
Wi-Fi STA
   ↓
HTTPS + X.509 verify
   ↓
ESP OTA app0/app1
   ↓
BLE OTA Status
   ↓
Restart
```

---

## Phase 0～2

### Phase 0：ESP-IDF Skeleton

- Native ESP-IDF project
- ESP32-S3
- 8MB Flash
- Octal PSRAM
- OTA partition table
- `app_main()`
- CI workflow

### Phase 1：Board

- GPIO1 Button
- GPIO21 LED
- 50ms debounce
- 2s long press
- `esp_timer_get_time()`
- FreeRTOS delay
- eFuse MAC serial

### Phase 2：PDM Mic

```text
GPIO42 CLK
GPIO41 DATA
16kHz / Mono / PCM16
320 samples / 20ms
```

使用 ESP-IDF 新 I2S PDM RX Channel API。

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
Max 160 bytes/frame
```

详细：`PHASE3_OPUS.md`

### Phase 4：Native NimBLE

已迁移：

- Advertising
- Connect / Disconnect
- GATT Server
- CCCD Subscribe
- MTU 517
- Omi Main Service
- Battery Service
- Device Information Service
- OTA Service

详细：`PHASE4_BLE_CORE.md`

### Phase 5：BLE Audio

协议保持：

```text
byte 0-1 packet index little-endian
byte 2   sub-index
byte 3+  Opus
```

实时策略：

- 16-frame queue
- 每轮最多 2 帧
- 队列满丢最旧
- 不补发陈旧语音

详细：`PHASE5_BLE_AUDIO.md`

---

## Phase 6～7：Camera / Photo BLE

### Phase 6：Camera

```text
VGA JPEG
Quality 25
XCLK 6MHz
PSRAM framebuffer
1 framebuffer
orientation = 180°
```

详细：`PHASE6_CAMERA.md`

### Phase 7：Photo BLE

保持原协议：

```text
首包：frame index + orientation + <=199 JPEG bytes
后续：frame index + <=200 JPEG bytes
结束：FF FF
```

优先级：

```text
Audio > Photo
```

详细：`PHASE7_PHOTO_BLE.md`

---

## Phase 8：Battery ADC

```text
GPIO2
ADC oneshot
12-bit
ADC_ATTEN_DB_12
10-sample average
Curve Fitting calibration
Voltage divider = 6.086
20s interval
```

BLE：

```text
Battery Service 0x180F
Battery Level   0x2A19
READ + NOTIFY
```

详细：`PHASE8_BATTERY_ADC.md`

---

## Phase 9：Power Management

使用 ESP-IDF v6.1 原生：

```text
DFS 80MHz ↔ 40MHz
PM Lock
FreeRTOS Tickless Idle
BLE Modem Sleep
Automatic Light Sleep
Deep Sleep
```

Deep Sleep 条件：

- 2 秒长按；
- Battery <= 3.3V；
- BLE 未连接且空闲 >= 5 分钟。

GPIO1 低电平唤醒。

Phase 11 已把 Power 关键时间/频率阈值统一迁入 `omiglass_config.h`，避免局部硬编码漂移。

详细：`PHASE9_POWER_MANAGEMENT.md`

---

## Phase 10：Wi-Fi / HTTPS OTA

### BLE 协议保持不变

命令：

```text
0x01 SET_WIFI
0x02 START_OTA
0x03 CANCEL_OTA
0x04 GET_STATUS
0x05 SET_URL
```

状态仍使用原 Omi `0x00 / 0x10...0x40 / 0xFF` 定义。

### Native ESP-IDF 实现

```text
esp_wifi
esp_netif
esp_event
esp_http_client
esp_https_ota
app_update
ESP x509 Certificate Bundle
```

### 安全变化

- HTTPS only；
- 删除 `setInsecure()`；
- 校验服务器证书；
- 校验 hostname；
- Wi-Fi credentials 仅 RAM；
- Password 不写日志；
- 完整 OTA URL 不写日志；
- 镜像不完整时不切换 boot partition。

### OTA Maintenance Mode

OTA 期间暂停：

```text
Mic
Opus realtime processing
BLE Audio TX
Photo Capture / Upload
```

保留：

```text
BLE OTA Control / Status
Wi-Fi HTTPS OTA
Battery
Power coordination
```

OTA 写 Flash 时遇到长按或 Critical Battery：

```text
cancel OTA first
 ↓
wait task exits safely
 ↓
then Deep Sleep
```

详细：

- `PHASE10_HTTPS_OTA.md`
- `PHASE10_CI_VALIDATION.md`

---

## Phase 11：整机功能对齐

Phase 11 中**能靠源码迁移、静态审计和 CI 自动化完成的部分已经收口**。

已经建立：

```text
PHASE11_SYSTEM_PARITY.md
PHASE11_SOFTWARE_CLOSURE.md
```

并把 Arduino 基线与 Native ESP-IDF 的外部行为逐项分为：

```text
MATCH
INTENTIONAL CHANGE
FIXED
NEED HW TEST
WATCH
```

### CI 自动保护

正式 `idf.py build` 前会运行：

```bash
python3 tools/check_phase11_parity.py
python3 tools/check_phase11_wire_behavior.py
python3 tools/check_phase11_runtime_contracts.py
```

自动保护：

- Device Info / GPIO / Mic / Opus / Battery / OTA 关键常量；
- Omi / Audio / Photo / OTA UUID；
- Audio `index LE + sub-index + Opus`；
- Photo 首包 199B / 后续 200B / 最大 202B / `FF FF`；
- OTA Control READ `[status,0]`；
- OTA Data READ/NOTIFY `[status,progress]`；
- OTA SET_WIFI / SET_URL payload 偏移与大端 URL length；
- Button / LED / Power 时间阈值；
- Audio > Photo 调度；
- OTA maintenance / cancel-before-sleep；
- Deep Sleep 资源释放与 GPIO1 wake；
- PM / Tickless / BLE Modem Sleep / Wi-Fi coexist / HTTPS-only Kconfig；
- Arduino `app.cpp` 的真实 Audio/Photo 字节行为基线。

### 已修复 / 已增强

```text
P11-001
OTA Control READ
Native 原先返回 [status, progress]
修复为 Arduino 实际行为 [status, 0]
```

```text
P11-002
Battery 增加同一次 ADC 采样的：
rawApprox / Curve Fitting / selected 三组电压诊断
供万用表真机裁决
```

Power 运行参数也已经从 `power_service.cpp` 局部数字统一到共享配置。

### 真机辅助脚本

```bash
python3 -m pip install -r tools/requirements-phase11.txt

# 基础 GATT / Device Info / Battery / OTA READ
python3 tools/phase11_ble_probe.py

# 60 秒 Audio
python3 tools/phase11_ble_probe.py --audio-seconds 60

# 单拍并重组 JPEG
python3 tools/phase11_ble_probe.py --photo

# 安全的 OTA GET_STATUS Notify
python3 tools/phase11_ble_probe.py --ota-status-notify

# 20 次连接/断开压力测试
python3 tools/phase11_ble_probe.py --reconnect-count 20
```

串口日志可以自动分析：

```bash
idf.py monitor | tee omiglass.log
python3 tools/phase11_serial_probe.py omiglass.log
```

会检查服务启动、panic、Mic 约 16k samples/s、Opus 约 50fps、Battery 双路径诊断和 Power State。

### 现在只剩真机裁决

1. Boot / Octal PSRAM；
2. GPIO1 Button / GPIO21 LED；
3. PDM Mic 电平、噪声、真实采样率；
4. 60s Opus + BLE Audio 解码质量；
5. VGA JPEG / Audio+Photo 并发；
6. 手机实际 ATT MTU / connection params / 20 次重连；
7. Battery 万用表 vs Arduino vs Native rawApprox/calibrated；
8. 3.4V / 3.3V 阈值；
9. DFS / Automatic Light Sleep / Deep Sleep 电流 / GPIO1 wake；
10. Wi-Fi + BLE coexistence；
11. HTTPS OTA 正常、失败、cancel、低电和长按安全路径。

这些项目无法再通过继续改源码“迁移掉”，必须接真实硬件验证。

---

## ESP-IDF v6.1 CI 验证

官方构建环境：

```text
espressif/idf:v6.1
```

真实执行：

```bash
idf.py set-target esp32s3
idf.py build
```

第一轮真实编译发现并已修复：

1. `ble_hs_util_ensure_addr()` 需要显式 `host/util/util.h`；
2. `uint32_t` 设备序列号使用 `%X` 被 `-Werror=format` 拒绝，已改用 `PRIX32`。

### 固件大小监控

最近确认值：

```text
Firmware        1,560,080 bytes
OTA Slot        1,835,008 bytes
Margin            274,928 bytes
Usage                  85%
```

因此当前仍保持原双 `0x1C0000` OTA App Slot，不在 Phase 11 修改分区布局。

CI 现在会：

- Phase 11 parity / wire / runtime contract checker；
- Python BLE/serial probe 语法检查；
- ESP-IDF v6.1 完整编译；
- 失败时生成 compiler error annotations；
- 成功时报告 firmware/slot/margin/usage；
- 同一分支自动取消过期构建。

---

## 当前验证边界

### 已确认

- Native 工程能在 ESP-IDF v6.1 完整编译；
- 当前 binary 能放入现有 OTA Slot；
- Phase 0～10 的 C/C++ / CMake / Kconfig 已进入真实 CI 编译闭环；
- Phase 11 手机端线协议已经进入自动 CI 防回归；
- Phase 11 Button/LED/Power/OTA maintenance 等运行时契约已经进入自动 CI 防回归；
- 真机测试已有 BLE 和串口自动探针。

### 仍待真机

- Boot / PSRAM；
- Button / LED；
- PDM Mic；
- Opus 实时链路；
- BLE 手机连接；
- Audio Notify；
- Camera；
- Photo BLE 重组；
- Battery ADC 与万用表对比；
- Automatic Light Sleep / DFS；
- Deep Sleep / GPIO1 wake；
- Wi-Fi + BLE coexistence；
- HTTPS server certificate validation；
- OTA 正常升级；
- OTA cancel / failure / low battery / long-press safety。

---

## 固定兼容性约束

除非后续显式修改并记录：

- ESP-IDF 固定 v6.1.x；
- XIAO ESP32S3 Sense；
- BLE Device Name：`OMI Glass`；
- Omi BLE UUID 不变；
- Audio Codec ID：21；
- 16kHz Mono；
- Opus 320 samples / 20ms；
- Audio BLE 包格式不变；
- Camera：VGA JPEG；
- Photo BLE 分包格式不变；
- Battery Service：`0x180F / 0x2A19`；
- OTA BLE Command / Status wire protocol 不变；
- Arduino `omiGlass/firmware/` 保持不动。

---

## 下一步

Phase 11 软件侧已经收口。下一步只有两条合理路径：

```text
A. 接 XIAO ESP32S3 Sense 跑 Phase 11 真机矩阵
```

或在真机验证通过之后：

```text
B. 进入 Phase 12 架构优化
```

在没有真机结果之前，不继续通过猜测修改协议或硬件行为。
