# Phase 11：整机功能对齐（System Parity）

日期：2026-10-22

分支：`feature/omiglass-esp-idf-migration`

基线：

- Arduino 参考实现：`omiGlass/firmware/`
- Native 实现：`omiGlass/esp32/`
- ESP-IDF：v6.1.x

## 1. Phase 11 目标

Phase 11 不再增加新业务功能，目标是把 Phase 0～10 已迁移的模块做一次整机级行为审计，区分：

- `MATCH`：协议/行为与 Arduino 基线一致；
- `INTENTIONAL CHANGE`：明确保留的修正或安全增强，不做 bug-for-bug 复制；
- `NEED FIX`：会影响客户端兼容或业务语义，需要修正；
- `NEED HW TEST`：源码/CI 无法证明，必须在 XIAO ESP32S3 Sense 真机上验证。

Phase 11 的完成条件不是“代码能编译”，而是：

1. 线协议逐字节对齐；
2. 关键运行时状态机对齐；
3. 有意差异全部有记录和理由；
4. 真机测试矩阵明确；
5. ESP-IDF v6.1 CI 持续保持通过。

---

## 2. 第一轮整机对齐矩阵

| 模块 | 检查项 | Arduino 基线 | Native ESP-IDF | 状态 | 处理 |
|---|---|---|---|---|---|
| Device Info | 设备名 | `OMI Glass` | `OMI Glass` | MATCH | 保持 |
| Device Info | Firmware Revision | `2.3.2` | `2.3.2` | MATCH | 保持 |
| Device Info | Hardware Revision | `ESP32-S3-v1.0` | `ESP32-S3-v1.0` | MATCH | 保持 |
| Device Info | Manufacturer | `Based Hardware` | `Based Hardware` | MATCH | 保持 |
| Device Info | Serial Number | eFuse MAC 数值格式 `%04X%08X` | 同一语义，使用 `PRIX32` | MATCH | 已通过 v6.1 CI |
| BLE | Omi Service UUID | `19B10000-...` | 相同 UUID | MATCH | 保持 |
| BLE | Audio Data UUID | `19B10001-...` | 相同 UUID | MATCH | 保持 |
| BLE | Audio Codec UUID | `19B10002-...` | 相同 UUID | MATCH | 保持 |
| BLE | Photo Data UUID | `19B10005-...` | 相同 UUID | MATCH | 保持 |
| BLE | Photo Control UUID | `19B10006-...` | 相同 UUID | MATCH | 保持 |
| BLE | OTA UUIDs | `...10/11/12` | 相同 UUID | MATCH | 保持 |
| BLE | Audio Data Properties | READ + NOTIFY | READ + NOTIFY | MATCH | 保持 |
| BLE | Audio Codec Properties | READ | READ | MATCH | 保持 |
| BLE | Photo Data Properties | READ + NOTIFY | READ + NOTIFY | MATCH | 保持 |
| BLE | Photo Control Properties | WRITE | WRITE | MATCH | 保持 |
| BLE | Battery Level | READ + NOTIFY | READ + NOTIFY | MATCH | 保持 |
| BLE | OTA Control | READ + WRITE | READ + WRITE | MATCH | 保持 |
| BLE | OTA Data | READ + NOTIFY | READ + NOTIFY | MATCH | 保持 |
| BLE | Preferred MTU | Arduino 常量 517，但未显式 `setMTU()` | Host Preferred MTU=517 | INTENTIONAL CHANGE | 保留，真机验证手机 MTU |
| BLE | Advertising interval | 旧代码没有调用 `setMinInterval/setMaxInterval` | 200～400ms | INTENTIONAL CHANGE | 保留；旧代码 `setMinPreferred/setMaxPreferred` 实际不是广告间隔 |
| BLE | Connection params | 旧代码只把 preferred range 放进广告数据 | 连接后主动请求 25～50ms / latency 0 / timeout 8s | INTENTIONAL CHANGE | 保留；优先保证 Audio/Photo 吞吐 |
| Audio | Codec ID | 21 | 21 | MATCH | 保持 |
| Audio | PCM | 16kHz / mono / 16-bit | 16kHz / mono / 16-bit | MATCH | NEED HW TEST | PDM 波形/增益 |
| Audio | Opus frame | 320 samples / 20ms | 320 samples / 20ms | MATCH | 保持 |
| Audio | Opus bitrate | 32kbps | 32kbps | MATCH | 保持 |
| Audio | Complexity | 3 | 3 | MATCH | 保持 |
| Audio | VBR | enabled | enabled | MATCH | 保持 |
| Audio | BLE packet header | index LE 2B + sub-index 1B | 相同 | MATCH | 保持 |
| Audio | sub-index | 0 | 0 | MATCH | 保持 |
| Audio | Queue when unsubscribed | 仍可能缓存旧 Opus 帧 | 不缓存，直接丢弃 | INTENTIONAL CHANGE | 保留，避免订阅后播放陈旧音频 |
| Audio | TX draining | 单 loop 尽量清空 ring buffer | 每轮限包，避免主循环长期被占用 | INTENTIONAL CHANGE | 保留，真机测吞吐/丢帧 |
| Photo | Camera resolution | VGA 640x480 | VGA 640x480 | MATCH | NEED HW TEST |
| Photo | JPEG quality | 25 | 25 | MATCH | NEED HW TEST |
| Photo | XCLK | 6MHz | 6MHz | MATCH | NEED HW TEST |
| Photo | Frame buffer | PSRAM / 1 buffer | PSRAM / 1 buffer | MATCH | NEED HW TEST |
| Photo | Grab mode | latest | latest | MATCH | NEED HW TEST |
| Photo | Orientation | 180° / byte `2` | byte `2` | MATCH | 保持 |
| Photo | 首包 | index=0 LE + orientation + <=199B JPEG | 相同 | MATCH | 保持 |
| Photo | 后续包 | index LE + <=200B JPEG | 相同 | MATCH | 保持 |
| Photo | 结束标记 | `FF FF` | `FF FF` | MATCH | 保持 |
| Photo | 每轮最多 chunk | 2 | 2 | MATCH | 保持 |
| Photo | Audio 优先 | Audio ring 非空时让路 | Audio queue 非空时让路 | MATCH | 保持 |
| Photo | 未订阅时抓图 | 只检查 BLE connected | 要求 Photo Notify 已订阅 | INTENTIONAL CHANGE | 保留，避免无接收端时占 framebuffer |
| Photo Control | `-1` | 单拍 | 单拍 | MATCH | 保持 |
| Photo Control | `0` | 停止 | 停止 | MATCH | 保持 |
| Photo Control | `>=5` | 固定使用 30s 配置周期 | 固定使用 30s 配置周期 | MATCH | 保持 |
| Battery | ADC GPIO | GPIO2 | GPIO2 | MATCH | NEED HW TEST |
| Battery | samples | 10 次 | 10 次 | MATCH | 保持 |
| Battery | sample spacing | 10ms | 10ms | MATCH | 保持 |
| Battery | divider | 6.086 | 6.086 | MATCH | NEED HW TEST |
| Battery | max/min | 4.2V / 3.2V | 4.2V / 3.2V | MATCH | 保持 |
| Battery | 平滑 | 差值 >5% 时每次移动 2% | 相同 | MATCH | 保持 |
| Battery | ADC 换算 | raw/4095*3.3V | 优先 ADC Curve Fitting，失败才 raw fallback | INTENTIONAL CHANGE | 真机与万用表校验后决定是否保留 |
| Button | GPIO | GPIO1 active-low | GPIO1 active-low | MATCH | NEED HW TEST |
| Button | debounce | 50ms | 50ms | MATCH | 保持 |
| Button | long press | 2s | 2s | MATCH | NEED HW TEST |
| LED | GPIO | GPIO21 inverted | GPIO21 inverted | MATCH | NEED HW TEST |
| LED | boot sequence | 150ms half-period / 1.5s | 相同 | MATCH | NEED HW TEST |
| LED | connected | solid ON | solid ON | MATCH | NEED HW TEST |
| LED | disconnected | 1s ON / 1s OFF | 相同 | MATCH | NEED HW TEST |
| Power | normal CPU | 80MHz | 80MHz | MATCH | NEED HW TEST |
| Power | idle CPU | 40MHz | 40MHz | MATCH | NEED HW TEST |
| Power | Light Sleep | 手工 light sleep，只有 connected 且空闲时 | ESP-IDF PM automatic light sleep | INTENTIONAL CHANGE | 真机验证 BLE 稳定性 |
| Power | long-press shutdown | 800ms LED 后直接 Deep Sleep | 800ms LED，等待按键释放再睡 | INTENTIONAL CHANGE | 保留，避免按键仍低电平导致立即唤醒 |
| Power | 5min offline Deep Sleep | config 定义但旧主循环未真正使用 | 已实现 | INTENTIONAL CHANGE | 真机验证广告/唤醒 |
| Power | critical battery shutdown | config 定义但旧主循环未真正使用 | <=3.3V 自动安全关机 | INTENTIONAL CHANGE | 真机校准 Battery 后验证 |
| OTA | commands | 01/02/03/04/05 | 相同 | MATCH | 保持 |
| OTA | status codes | 00/10-12/20-22/30-32/40/FF | 相同 | MATCH | 保持 |
| OTA | SET_URL length | 2-byte big-endian | 相同 | MATCH | 保持 |
| OTA | status notify | `[status, progress]` | `[status, progress]` | MATCH | 保持 |
| OTA | OTA Control READ | 实际 app.cpp 回调为 `[status, 0]` | 当前为 `[status, progress]` | NEED FIX | Phase 11 修复 |
| OTA | OTA Data READ | characteristic 保存最近 `[status, progress]` | `[status, progress]` | MATCH | 保持 |
| OTA | HTTP | 允许 | 禁止 | INTENTIONAL CHANGE | 安全增强，不回退 |
| OTA | HTTPS cert | `setInsecure()` | X.509 Root CA Bundle + hostname verify | INTENTIONAL CHANGE | 安全增强，不回退 |
| OTA | credentials | RAM | RAM | MATCH | 保持 |
| OTA | URL log | 完整 URL | 不记录完整 URL | INTENTIONAL CHANGE | 防止 token 泄露 |
| OTA | OTA + shutdown | 可能直接关机 | 先 cancel OTA 再 sleep | INTENTIONAL CHANGE | 保留，避免写 Flash 中断 |
| Build | ESP-IDF v6.1 | N/A | CI `idf.py build` success | MATCH TARGET | 持续要求 |
| Size | app image | Arduino 未纳入当前 CI | Native 1,560,080B / slot 1,835,008B | WATCH | 85%，持续监控 |

---

## 3. 第一轮确定需要修复的问题

### P11-001：OTA Control READ 第二字节

Arduino 实际生效的 `app.cpp::OTAControlCallback::onRead()`：

```text
[ota_get_status(), 0]
```

Native 当前：

```text
[status, progress]
```

由于 OTA Control 是客户端可直接读取的 GATT Characteristic，这是线协议可观察差异，应修复为：

```text
OTA Control READ -> [status, 0]
OTA Data READ    -> [status, progress]
OTA Data NOTIFY  -> [status, progress]
```

### P11-002：Battery ADC 的校准语义需要真机裁决

旧固件使用：

```text
raw / 4095 * 3.3V * 6.086
```

Native 优先使用 ESP-IDF ADC Curve Fitting，再乘 6.086。

`6.086` 本身是旧固件按实测校准出来的 divider ratio，因此“ADC 校准 + 旧 divider 校准值”可能造成系统性偏差。源码无法证明哪一种更准确。

Phase 11 真机测试必须同时记录：

```text
万用表实际电池电压
Arduino 报告电压
Native calibrated 电压
Native raw fallback 电压
```

再决定 Phase 11 是否回到 raw-compatible 计算。

---

## 4. 第一轮明确保留的有意差异

以下不做 bug-for-bug 回退：

1. HTTPS 必须校验证书，不恢复 `setInsecure()`；
2. 不允许 HTTP OTA；
3. OTA 写 Flash 期间不直接 Deep Sleep；
4. 长按关机等待 GPIO1 释放后再进入低电平唤醒 Deep Sleep；
5. 未订阅 Audio 时不缓存陈旧语音；
6. 未订阅 Photo 时不占用 Camera framebuffer；
7. Native 主动请求适合实时 Audio/Photo 的 BLE 连接参数；
8. OTA URL/密码不写日志。

这些属于安全性、实时性或明显历史 API 误用修正，不应为了“完全复制旧代码”而退化。

---

## 5. Phase 11 真机验证矩阵

### BLE / 手机

- 扫描能发现 `OMI Glass`；
- Service / Characteristic UUID 全部一致；
- Properties 一致；
- 手机订阅 Audio / Photo / Battery / OTA Notify 成功；
- ATT MTU 实际协商值 >= 205，目标接近 517；
- BLE 断开后自动重新广播；
- 连续重连 20 次无死锁/重启。

### Audio

- PDM Mic 16kHz 单声道稳定；
- Opus 每 20ms 一帧；
- 手机可持续解码；
- packet index 连续递增并允许 uint16 wrap；
- sub-index 恒为 0；
- 60 秒连续音频无明显积压；
- Camera 上传同时开启时 Audio 优先且可用。

### Photo

- 首包前三字节正确；
- 后续 chunk index little-endian；
- 单 chunk 最大 202B；
- orientation=2；
- JPEG 可完整重组；
- 末尾 `FF FF`；
- 单拍、停止、周期拍照语义一致；
- Audio 活跃时图片允许变慢但不能破坏。

### Battery

- 万用表与固件电压误差；
- Arduino 与 Native 同一电压下百分比差值；
- 20 秒采样周期；
- Battery Level READ / Notify；
- 3.4V / 3.3V 阈值行为。

### Power

- 开机 LED；
- BLE connected/disconnected LED；
- 2 秒长按；
- Deep Sleep 电流；
- GPIO1 唤醒；
- BLE connected 下自动 Light Sleep 不掉链；
- Audio / Photo 期间不进入 Light Sleep；
- 5 分钟离线 Deep Sleep。

### OTA

- 正确 Wi-Fi；
- 错密码；
- DNS 失败；
- TLS 证书失败；
- HTTP URL 拒绝；
- 404；
- 截断镜像；
- CANCEL；
- OTA 中长按关机；
- OTA 中临界低电；
- 成功升级并重启；
- 新 Firmware Revision 可读取。

---

## 6. CI 自动对齐检查

Phase 11 新增：

```text
tools/check_phase11_parity.py
```

自动比较 Arduino `config.h` 与 Native `omiglass_config.h` 的关键硬件/协议参数。

目的不是替代真机测试，而是防止 Phase 12 重构时无意修改：

- Device metadata；
- Mic 参数；
- Opus 参数；
- Camera 参数；
- Battery 参数；
- Button / LED GPIO；
- OTA command/status code。

---

## 7. 当前 Phase 11 状态

```text
源码行为审计           IN PROGRESS
协议常量自动检查       IN PROGRESS
P11-001 OTA READ       NEED FIX
Battery ADC 真机裁决   NEED HW TEST
BLE 真机联调           NEED HW TEST
Audio 真机联调         NEED HW TEST
Photo 真机联调         NEED HW TEST
Power 真机联调         NEED HW TEST
HTTPS OTA 真机联调     NEED HW TEST
```

Phase 11 在完成真机矩阵前不会标记为 COMPLETE。
