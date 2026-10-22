# Phase 11：整机功能对齐（System Parity）

日期：2026-10-22  
分支：`feature/omiglass-esp-idf-migration`

基线：

- Arduino 参考实现：`omiGlass/firmware/`
- Native 实现：`omiGlass/esp32/`
- ESP-IDF：v6.1.x

## 1. Phase 11 目标

Phase 11 不再扩展业务功能，而是对 Phase 0～10 做整机行为审计。

状态定义：

- `MATCH`：外部协议或行为与 Arduino 基线一致；
- `INTENTIONAL CHANGE`：明确保留的修正、安全增强或实时性优化；
- `FIXED`：Phase 11 已发现并修复的兼容差异；
- `NEED HW TEST`：源码与 CI 无法证明，必须在 XIAO ESP32S3 Sense 真机验证；
- `WATCH`：当前可用，但要持续监控。

Phase 11 完成条件：

1. BLE / Audio / Photo / OTA 线协议完成逐字节审计；
2. Button / LED / Battery / Power 状态机完成行为审计；
3. 所有有意差异都有记录，不做无意义的 bug-for-bug 复制；
4. Native ESP-IDF v6.1 CI 持续通过；
5. 真机验证矩阵完成。

> Phase 11 当前为 **IN PROGRESS**。源码/CI 对齐不能替代真机验证。

---

## 2. 当前整机对齐矩阵

### 2.1 Device Info / BLE GATT

| 检查项 | Arduino | Native | 状态 |
|---|---|---|---|
| Device Name | `OMI Glass` | `OMI Glass` | MATCH |
| Firmware Revision | `2.3.2` | `2.3.2` | MATCH |
| Hardware Revision | `ESP32-S3-v1.0` | 相同 | MATCH |
| Manufacturer | `Based Hardware` | 相同 | MATCH |
| Serial | eFuse MAC `%04X%08X` 语义 | 同一语义，portable `PRIX32` | MATCH |
| Omi Service UUID | `19B10000-...` | 相同 | MATCH |
| Audio Data / Codec UUID | `...01 / ...02` | 相同 | MATCH |
| Photo Data / Control UUID | `...05 / ...06` | 相同 | MATCH |
| OTA Service / Control / Data | `...10 / ...11 / ...12` | 相同 | MATCH |
| Audio Data | READ + NOTIFY | READ + NOTIFY | MATCH |
| Audio Codec | READ | READ | MATCH |
| Photo Data | READ + NOTIFY | READ + NOTIFY | MATCH |
| Photo Control | WRITE | WRITE | MATCH |
| Battery Level | READ + NOTIFY | READ + NOTIFY | MATCH |
| OTA Control | READ + WRITE | READ + WRITE | MATCH |
| OTA Data | READ + NOTIFY | READ + NOTIFY | MATCH |
| Preferred MTU | 常量 517，但旧代码未显式 setMTU | Host Preferred MTU 517 | INTENTIONAL CHANGE / NEED HW TEST |
| Advertising interval | 旧代码误用 `setMinPreferred/MaxPreferred`，未直接设广播 interval | 200～400ms | INTENTIONAL CHANGE |
| Connection params | 只广告 preferred range | 连接后请求 25～50ms / latency 0 / timeout 8s | INTENTIONAL CHANGE / NEED HW TEST |

说明：旧 NimBLE-Arduino 的 `setMinPreferred()` / `setMaxPreferred()` 设置的是 advertised preferred connection interval，而不是 advertising interval。Native 不复制这一历史 API 误用。

---

### 2.2 PDM Mic / Opus / Audio BLE

| 检查项 | Arduino | Native | 状态 |
|---|---|---|---|
| Mic GPIO | CLK42 / DATA41 | 相同 | MATCH |
| PCM | 16kHz / mono / 16-bit | 相同 | MATCH / NEED HW TEST |
| Gain | x2 + int16 saturation | 相同 | MATCH |
| DMA | 8 x 256 | 相同 | MATCH |
| Mic read granularity | 给 1600-sample buffer，20ms timeout | 每次请求 320 samples，30ms timeout | INTENTIONAL CHANGE / NEED HW TEST |
| Opus application | VOIP | VOIP | MATCH |
| Frame | 320 samples / 20ms | 相同 | MATCH |
| Bitrate | 32kbps | 相同 | MATCH |
| Complexity | 3 | 3 | MATCH |
| Signal | VOICE | VOICE | MATCH |
| VBR / constrained VBR | on / off | 相同 | MATCH |
| LSB depth | 16 | 16 | MATCH |
| DTX / FEC / loss | 0 / 0 / 0 | 相同 | MATCH |
| PCM ring full | 丢最旧 sample | 相同 | MATCH |
| BLE header | index LE 2B + sub-index 1B | 相同 | MATCH |
| sub-index | 0 | 0 | MATCH |
| 未订阅时 | 仍可能缓存旧 Opus 包 | 直接丢弃 | INTENTIONAL CHANGE |
| TX queue full | 旧实现跳过新 packet | Native 丢最旧 frame，保实时性 | INTENTIONAL CHANGE |
| Notify 失败 | 旧代码仍递增 packet index | 仅成功后递增 | INTENTIONAL CHANGE |
| MTU 不足 | 旧代码仍尝试 notify | Native 丢本帧并等待 MTU | INTENTIONAL CHANGE |

Mic 的读取粒度不同不会改变最终 320-sample Opus frame，但可能影响 DMA 调度、延迟和采样稳定性，因此留给真机测试裁决。

---

### 2.3 Camera / Photo BLE

| 检查项 | Arduino | Native | 状态 |
|---|---|---|---|
| Camera pins | XIAO ESP32S3 Sense 原映射 | 相同 | MATCH |
| XCLK | 6MHz | 6MHz | MATCH / NEED HW TEST |
| Format | JPEG | JPEG | MATCH |
| Resolution | VGA 640x480 | VGA | MATCH |
| JPEG quality | 25 | 25 | MATCH |
| Framebuffer | PSRAM / 1 | PSRAM / 1 | MATCH |
| Grab mode | latest | latest | MATCH |
| Orientation | 180° / byte 2 | byte 2 | MATCH |
| 首包 | `index=0 LE + orientation + <=199 JPEG` | 相同 | MATCH |
| 后续包 | `index LE + <=200 JPEG` | 相同 | MATCH |
| 单包最大 | 202 bytes | 202 bytes | MATCH |
| 结束标记 | `FF FF` | `FF FF` | MATCH |
| Audio priority | 有待发 Audio 时让路 | 相同 | MATCH |
| 未订阅 Photo | 连接后仍可能抓图 | 不抓图 | INTENTIONAL CHANGE |
| Notify / MTU 错误 | 旧代码继续推进发送位置 | Native 不把失败发送当成功 | INTENTIONAL CHANGE |

Photo 线协议保持完全一致；Native 只避免在没有接收端时占用 framebuffer，并增强发送失败处理。

---

### 2.4 Battery

| 检查项 | Arduino | Native | 状态 |
|---|---|---|---|
| ADC GPIO | GPIO2 | GPIO2 | MATCH |
| sample count | 10 | 10 | MATCH |
| sample spacing | 10ms | 10ms | MATCH |
| divider ratio | 6.086 | 6.086 | MATCH / NEED HW TEST |
| min / max | 3.2V / 4.2V | 相同 | MATCH |
| smoothing | 差值 >5% 时每轮只移动 2% | 相同 | MATCH |
| ADC conversion | raw/4095*3.3V | 优先 ADC Curve Fitting；失败回 raw | INTENTIONAL CHANGE / NEED HW TEST |
| critical threshold | config 有 3.3V，但旧主循环未执行自动关机 | <=3.3V 安全关机 | INTENTIONAL CHANGE / NEED HW TEST |
| low threshold | config 3.4V | 参与 Native power policy | INTENTIONAL CHANGE / NEED HW TEST |

### P11-002：Battery ADC 真机裁决

`6.086` 是旧固件已有的实测 divider ratio，而 Native 又增加 ADC Curve Fitting。两者叠加是否导致系统性偏差不能只靠源码判断。

真机必须同时记录：

```text
万用表实际电池电压
Arduino 报告电压
Native calibrated 电压
Native raw fallback 电压
```

然后再决定是否保留 Curve Fitting 路径。

---

### 2.5 Button / LED / Power

| 检查项 | Arduino | Native | 状态 |
|---|---|---|---|
| Button | GPIO1 active-low | 相同 | MATCH / NEED HW TEST |
| debounce | 50ms | 50ms | MATCH |
| long press | 2s | 2s | MATCH |
| LED | GPIO21 inverted | 相同 | MATCH |
| Boot LED | 1.5s / 150ms half-period | 相同 | MATCH |
| Connected LED | solid ON | 相同 | MATCH |
| Disconnected LED | 1s ON / 1s OFF | 相同 | MATCH |
| Power-off LED | 800ms / 200ms half-period | 相同 | MATCH |
| Normal CPU | 80MHz | 80MHz | MATCH / NEED HW TEST |
| Idle CPU | 40MHz | 40MHz | MATCH / NEED HW TEST |
| Audio active Light Sleep | 禁止 | 禁止 | MATCH |
| Light Sleep implementation | 手工 `esp_light_sleep_start()` | ESP-IDF PM automatic light sleep | INTENTIONAL CHANGE / NEED HW TEST |
| long-press shutdown | LED sequence 后直接 sleep | 等 GPIO1 释放后再 sleep | INTENTIONAL CHANGE |
| 5min offline Deep Sleep | 常量存在但旧 loop 未执行 | 已实现 | INTENTIONAL CHANGE / NEED HW TEST |
| critical battery shutdown | 常量存在但旧 loop 未执行 | 已实现 | INTENTIONAL CHANGE / NEED HW TEST |

等待 GPIO1 释放后再进入低电平唤醒 Deep Sleep 是有意修复，避免“长按关机时按键仍为低电平 → 立即重新唤醒”。

---

### 2.6 OTA

| 检查项 | Arduino | Native | 状态 |
|---|---|---|---|
| Commands | 01/02/03/04/05 | 相同 | MATCH |
| Status codes | 00/10-12/20-22/30-32/40/FF | 相同 | MATCH |
| SET_WIFI format | `[cmd,ssid_len,ssid,pass_len,pass]` | 相同 | MATCH |
| SET_URL length | 2-byte big-endian | 相同 | MATCH |
| OTA Data READ / NOTIFY | `[status, progress]` | 相同 | MATCH |
| OTA Control READ | `[status, 0]` | `[status, 0]` | **FIXED P11-001** |
| duplicate START | 静默忽略 | 重发当前 status/progress | INTENTIONAL CHANGE |
| busy SET_WIFI / SET_URL | 旧版允许修改全局数据 | Native 拒绝并 ERROR | INTENTIONAL CHANGE |
| HTTP | 允许 | 拒绝 | INTENTIONAL CHANGE |
| HTTPS validation | `setInsecure()` | Root CA Bundle + hostname verify | INTENTIONAL CHANGE |
| credentials | RAM | RAM | MATCH |
| URL logging | 完整 URL | 不记录完整 URL | INTENTIONAL CHANGE |
| OTA + shutdown | 可能直接进入关机路径 | 先 cancel OTA，等待 Task 退出 | INTENTIONAL CHANGE |

P11-001 已修复：

```text
OTA Control READ -> [status, 0]
OTA Data READ    -> [status, progress]
OTA Data NOTIFY  -> [status, progress]
```

---

## 3. Phase 11 自动化保护

### 3.1 Source parity checker

`tools/check_phase11_parity.py`

在 CI 编译前自动比较 Arduino `config.h` 与 Native `omiglass_config.h`：

- Device metadata；
- GPIO；
- Mic；
- Opus；
- BLE MTU / connection parameters；
- Camera pins / XCLK / JPEG quality；
- Battery；
- OTA command / status；
- Omi / Audio / Photo / OTA UUID。

它同时接受 Native 常量使用 `#define` 或 `constexpr`，避免测试工具绑定某一种 C++ 表达形式。

### 3.2 Wire behavior checker

`tools/check_phase11_wire_behavior.py`

锁定：

- OTA Control `[status,0]`；
- OTA Data `[status,progress]`；
- Audio packet index little-endian；
- Audio sub-index=0；
- Photo chunk index little-endian；
- Photo orientation=2；
- Photo 199/200/202 byte 约束；
- Photo `FF FF` 结束标记。

这些检查的目的，是防止 Phase 12 重构时无意改变手机端线协议。

---

## 4. Phase 11 真机 BLE 自动探针

脚本：

```text
tools/phase11_ble_probe.py
```

Host 依赖：

```bash
python3 -m pip install -r tools/requirements-phase11.txt
```

默认只读检查：

```bash
python3 tools/phase11_ble_probe.py
```

会自动检查：

- 扫描 / 连接 `OMI Glass`；
- 4 个关键 Service；
- Characteristic UUID / Properties；
- Audio Codec ID=21；
- Battery Level；
- Device Info；
- Serial Number；
- OTA Control READ `[status,0]`；
- OTA Data READ `[status,progress]`。

采集实时 Audio：

```bash
python3 tools/phase11_ble_probe.py --audio-seconds 60
```

检查 packet index、sub-index 和基本帧完整性。

触发并重组一次 Photo：

```bash
python3 tools/phase11_ble_probe.py --photo
```

检查 chunk index、orientation、最大包长、`FF FF` end marker、JPEG `FF D8 ... FF D9`。

两项一起：

```bash
python3 tools/phase11_ble_probe.py --audio-seconds 60 --photo
```

注意：Linux BlueZ 后端的 `BleakClient.mtu_size` 可能只报告 23，因此低 MTU 只作为 WARN；最终真实 ATT MTU 应结合 ESP32 log / 手机端确认。

---

## 5. 真机验证矩阵

### BLE

- 扫描发现 `OMI Glass`；
- Service / Characteristic / Properties 全对；
- Audio / Photo / Battery / OTA Notify 可订阅；
- 实际 ATT MTU 足够承载最大 202-byte Photo packet；
- 连续断开/重连 20 次；
- 连接参数在手机接受后稳定。

### Audio

- PDM 16kHz mono 波形正常；
- x2 gain 无异常饱和；
- 60s 连续 Opus 可解码；
- packet index 连续；
- sub-index 恒 0；
- Photo 同时传输时 Audio 保持优先；
- 统计 dropped / notify error / queue depth。

### Photo

- VGA JPEG 可完整重组；
- orientation=2；
- 首包 / 后续包 / end marker 正确；
- 单拍 / 停止 / 30s 周期拍照；
- Audio 活跃时图片可变慢但不能损坏。

### Battery

- 万用表对比；
- Arduino 与 Native 同板对比；
- 20s 更新；
- READ / NOTIFY；
- 3.4V / 3.3V 阈值行为。

### Button / Power

- Boot / connected / disconnected / power-off LED；
- short press；
- 2s long press；
- Deep Sleep 电流；
- GPIO1 唤醒；
- BLE idle Light Sleep 稳定性；
- Audio/Photo 时禁止 Light Sleep；
- 5min offline Deep Sleep。

### OTA

- 正确 Wi-Fi；
- 错密码；
- DNS 失败；
- TLS 证书失败；
- HTTP URL 拒绝；
- 404；
- 截断镜像；
- CANCEL；
- OTA 中长按；
- OTA 中 critical battery；
- 成功升级、重启、新 Firmware Revision。

---

## 6. Firmware size WATCH

最近已验证的 Native image：

```text
1,560,080 bytes
```

当前单 OTA slot：

```text
0x1C0000 = 1,835,008 bytes
```

余量：

```text
274,928 bytes
```

使用率约：

```text
85%
```

Phase 11 不修改分区表，但 CI 持续报告 image size；Phase 12 如继续增长，应单独设计完整 8MB Flash 的 A/B OTA 分区迁移方案。

---

## 7. 当前状态

```text
整机源码审计                 IN PROGRESS
Device Info / GATT            AUDITED
BLE UUID / Properties         AUDITED
Audio wire protocol           AUDITED
Opus encoder                  AUDITED
PDM Mic                       AUDITED / NEED HW TEST
Camera config                 AUDITED / NEED HW TEST
Photo wire protocol           AUDITED
Battery                       AUDITED / NEED HW TEST
Button / LED                  AUDITED / NEED HW TEST
Power                         AUDITED / NEED HW TEST
OTA protocol                  AUDITED
P11-001 OTA Control READ      FIXED
P11-002 Battery calibration   NEED HW TEST
Source parity CI              ENABLED
Wire behavior CI              ENABLED
Host BLE probe                ADDED
ESP-IDF v6.1 build            MUST REMAIN GREEN
```

Phase 11 在真机验证完成前不会标记为 COMPLETE。
