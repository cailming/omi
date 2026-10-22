# Phase 11：软件侧收口说明

日期：2026-10-22  
分支：`feature/omiglass-esp-idf-migration`

## 1. 结论

Phase 11 中**不依赖真机、可以仅靠源码/CI继续完成的部分已经基本收口**。

现在仓库同时具备：

- Arduino `config.h` ↔ Native 配置常量 parity；
- Arduino `app.cpp` ↔ Native Audio/Photo 实际字节行为保护；
- OTA Command payload 字节级保护；
- Button / LED / Power / Deep Sleep / OTA 维护模式运行时契约检查；
- ESP-IDF v6.1 完整构建；
- BLE 真机自动探针；
- 串口运行日志自动探针；
- Battery raw approximation / Curve Fitting 双路径诊断。

因此 Phase 11 后续不应再为了“看起来还有工作”继续加入模拟功能；剩余问题需要真实 XIAO ESP32S3 Sense、手机、万用表和真实 Wi-Fi/HTTPS 环境裁决。

---

## 2. Power 参数已统一到共享配置

过去 `power_service.cpp` 内部还存在局部硬编码：

```text
80MHz
40MHz
5s Light Sleep idle
45s Power Save
5min Deep Sleep
800ms Power-off LED
200ms half period
1s Low Battery blink
```

现在全部统一迁入：

```text
main/omiglass_config.h
```

对应常量：

```text
OMI_NORMAL_CPU_FREQ_MHZ
OMI_MIN_CPU_FREQ_MHZ
OMI_POWER_LIGHT_SLEEP_IDLE_MS
OMI_POWER_SAVE_IDLE_MS
OMI_POWER_DEEP_SLEEP_IDLE_MS
OMI_POWER_OFF_SEQUENCE_MS
OMI_POWER_OFF_BLINK_HALF_PERIOD_MS
OMI_LOW_BATTERY_BLINK_HALF_PERIOD_MS
```

这样 Phase 12 重构时不会因为 `power_service.cpp` 内局部数字变化导致行为漂移。

---

## 3. CI 自动检查层

### 3.1 `check_phase11_parity.py`

检查 Arduino 配置与 Native 配置：

- Device metadata；
- GPIO；
- Mic；
- Opus；
- BLE；
- Camera；
- Battery；
- OTA command/status；
- UUID。

### 3.2 `check_phase11_wire_behavior.py`

锁定手机端可见的字节协议：

```text
Audio index little-endian
Audio sub-index = 0
Photo index little-endian
Photo orientation = 2
Photo 199/200/202 bytes
Photo FF FF terminator
OTA Control READ = [status,0]
OTA Data READ/Notify = [status,progress]
```

同时锁定 OTA Command payload：

```text
SET_WIFI:
[cmd][ssid_len][ssid...][pass_len][pass...]

SET_URL:
[cmd][url_len_hi][url_len_lo][url...]
```

并检查 Arduino `app.cpp` 的真实发送代码没有发生未审计变化。

### 3.3 `check_phase11_runtime_contracts.py`

锁定：

- 50ms debounce；
- 2s long press；
- Boot LED 1.5s / 150ms；
- Power-off LED 800ms / 200ms；
- 80/40MHz；
- 45s Power Save；
- 5min Deep Sleep；
- Audio 主循环优先于 Photo；
- Photo 内部再次检查 Audio pending；
- OTA maintenance mode 在实时数据面之前处理；
- OTA 中长按/低电先 cancel；
- Deep Sleep 前释放 OTA/Audio/Photo/Mic/Opus/Camera/Battery；
- GPIO1 low-level wake；
- PM / Tickless / BLE Modem Sleep / Wi-Fi coexist / HTTPS-only Kconfig。

---

## 4. 真机 BLE 探针

文件：

```text
tools/phase11_ble_probe.py
```

基础只读检查：

```bash
python3 tools/phase11_ble_probe.py
```

Audio：

```bash
python3 tools/phase11_ble_probe.py --audio-seconds 60
```

Photo：

```bash
python3 tools/phase11_ble_probe.py --photo
```

安全验证 OTA GET_STATUS Notify：

```bash
python3 tools/phase11_ble_probe.py --ota-status-notify
```

它只发送：

```text
0x04 GET_STATUS
```

不会设置 Wi-Fi，不会修改 URL，也不会启动 OTA。

20 次断开/重连：

```bash
python3 tools/phase11_ble_probe.py --reconnect-count 20
```

每次重新连接后都会读取 Audio Codec ID，验证 GATT Server 在断开、重新广播、重连之后仍可使用。

组合测试：

```bash
python3 tools/phase11_ble_probe.py \
  --ota-status-notify \
  --audio-seconds 60 \
  --photo \
  --reconnect-count 20
```

---

## 5. 串口运行日志探针

文件：

```text
tools/phase11_serial_probe.py
```

先采集：

```bash
idf.py monitor | tee omiglass.log
```

再检查：

```bash
python3 tools/phase11_serial_probe.py omiglass.log
```

自动检查：

- Phase 2～10 关键服务是否全部启动；
- 是否出现 Guru Meditation / assert / panic / Backtrace；
- Mic 每秒采样量是否大致在 16kHz 附近；
- PCM peak 是否不是全 0；
- Opus 是否大致在 50 frame/s；
- Battery 是否输出 rawApprox / calibrated 双路径；
- Battery 百分比是否在 0~100；
- 观察到哪些 Power State；
- BLE Audio notify error 统计。

仅检查启动：

```bash
python3 tools/phase11_serial_probe.py omiglass.log --boot-only
```

如果某块板确实没有可用 ADC calibration eFuse，可临时允许：

```bash
python3 tools/phase11_serial_probe.py omiglass.log --allow-no-calibration
```

---

## 6. Arduino 中“有定义但旧主流程并未真正执行”的配置

迁移时不能看到 `config.h` 有宏就机械复制。下面这些值需要区分“配置存在”和“运行行为存在”。

| Arduino 配置 | 旧主流程实际情况 | Native 处理 |
|---|---|---|
| `MAX_CPU_FREQ_MHZ=100` | 主流程只调用 `NORMAL_CPU_FREQ_MHZ=80` | 不复制未使用 100MHz；Native PM 上限 80MHz |
| `LIGHT_SLEEP_DURATION_US=50000` | `enableLightSleep()` 实际动态计算 5~15s，并未使用该宏 | 不复制；使用 ESP-IDF automatic Light Sleep |
| `DEEP_SLEEP_THRESHOLD_MS=300000` | Arduino loop 未真正实现离线 5min 自动 Deep Sleep | Native 作为明确 Power 策略实现，标记 Intentional Change |
| `BATTERY_REPORT_INTERVAL_MS=90000` | 主循环使用 `BATTERY_TASK_INTERVAL_MS=20000` | 保留常量但业务仍按 20s 更新 |
| `CAMERA_TASK_INTERVAL_MS` / stack / priority | Arduino 当前没有独立 Camera Task | 不机械创建任务 |
| `CAMERA_POWER_DOWN_DELAY_MS` | 当前 app.cpp 没有按该值关闭 Camera | 不复制未执行行为 |
| `BLE_CHUNK_SIZE=500` | 实际 Photo 发送固定约 200B | Native 保持真实 199/200B 协议 |
| `BLE_PHOTO_TRANSFER_DELAY` | 当前 Photo 发送逻辑未使用该宏 | 不复制 |
| `BLE_TX_POWER=ESP_PWR_LVL_N0` | 当前 `app.cpp` 未发现 `BLEDevice::setPower()` 调用 | 不凭空改变 Native TX power |
| `BLE_ADV_TIMEOUT_MS` | 广播实际永久运行 | Native 使用 `BLE_HS_FOREVER` |
| `BLE_SLEEP_ADV_INTERVAL` | 当前主流程未使用 | 不复制 |
| `BLE_CONNECTION_TIMEOUT_MS=0` | 当前主流程无自动断开定时器 | Native 同样不加应用层连接超时 |
| `BOOT_COMPLETE_DELAY_MS=3000` | 实际 LED Boot sequence 硬编码为 1500ms | Native 跟随真实 app.cpp：1500ms |
| `LED_BOOT_BLINK_FAST=200` | 实际 Boot LED 半周期是 150ms | Native 跟随真实 app.cpp：150ms |
| `STATUS_REPORT_INTERVAL_MS` | 当前主流程无对应报告任务 | 不复制未执行功能 |

这张表的目的，是防止后续审阅者把“没有迁移未使用宏”误判成漏功能。

---

## 7. 现在真正只剩真机才能裁决的项目

### 硬件/物理层

- Boot + Octal PSRAM；
- GPIO1 Button；
- GPIO21 LED；
- PDM Mic 电平、噪声、实际采样率；
- Camera XCLK 6MHz 真机稳定性；
- Deep Sleep 实际电流；
- DFS / Automatic Light Sleep 实际功耗。

### 手机 BLE

- 手机实际接受的 ATT MTU；
- connection interval 最终值；
- Audio 60s 解码质量；
- Photo JPEG 完整性；
- Audio + Photo 并发；
- 20 次重连。

### Battery

- 万用表 vs Arduino vs Native rawApprox vs Native calibrated；
- 3.4V low battery；
- 3.3V critical shutdown。

### OTA / 网络

- Wi-Fi + BLE coexistence；
- 正确 HTTPS OTA；
- 错密码 / DNS / TLS / 404 / 截断镜像；
- CANCEL；
- OTA 中长按；
- OTA 中 critical battery；
- 重启进入新分区。

这些项目无法通过继续改源码“迁移掉”；再改只能变成没有硬件证据的猜测。
