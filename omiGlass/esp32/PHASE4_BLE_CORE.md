# Phase 4：ESP-IDF v6.1 原生 NimBLE BLE Core / GATT

## 目标

本阶段只完成 BLE Core 和 GATT 结构迁移，不发送实时 Opus 音频。

明确边界：

- 完成 NimBLE 初始化。
- 完成广播。
- 完成连接 / 断开事件。
- 完成 MTU 事件。
- 完成通知订阅状态识别。
- 完成原 Arduino GATT Service / Characteristic 结构迁移。
- 保留原 UUID、权限和设备信息。
- 不在 `on_opus_encoded()` 中发送 BLE notify。
- 不实现 Camera / Battery ADC / OTA 的真实业务行为。

## ESP-IDF 基线

固定版本：

```text
ESP-IDF v6.1.x
```

`main/idf_component.yml` 已限制：

```yaml
idf:
  version: ">=6.1,<6.2"
```

## NimBLE 角色

`sdkconfig.defaults` 当前只启用眼镜需要的角色：

```text
Peripheral   ON
Broadcaster  ON
GATT Server  ON
Central      OFF
Observer     OFF
GATT Client  OFF
```

最大并发连接：

```text
1
```

Preferred ATT MTU：

```text
517
```

## GATT 服务结构

### 1. Omi Main Service

Service UUID：

```text
19B10000-E8F2-537E-4F6C-D104768A1214
```

Characteristics：

| Characteristic | UUID | 权限 | Phase 4 行为 |
| --- | --- | --- | --- |
| Audio Data | `19B10001-E8F2-537E-4F6C-D104768A1214` | READ + NOTIFY | 可订阅，但不发送实时音频 |
| Audio Codec | `19B10002-E8F2-537E-4F6C-D104768A1214` | READ | 返回 Codec ID 21 |
| Photo Data | `19B10005-E8F2-537E-4F6C-D104768A1214` | READ + NOTIFY | 可订阅，暂不发送图片 |
| Photo Control | `19B10006-E8F2-537E-4F6C-D104768A1214` | WRITE | 接收并记录控制值，不执行拍照 |

### 2. Battery Service

Service：

```text
0x180F
```

Battery Level：

```text
0x2A19
READ + NOTIFY
```

Phase 8 迁移 ADC 之前暂时返回占位值 0。

### 3. Device Information Service

Service：

```text
0x180A
```

Characteristics：

```text
0x2A29 Manufacturer Name
0x2A24 Model Number
0x2A26 Firmware Revision
0x2A27 Hardware Revision
0x2A25 Serial Number
```

序列号由 Phase 1 的 `board_service` 生成，由 `app_main()` 传给 BLE 服务。

BLE 层不依赖序列号的具体生成方式。

### 4. OTA Service

Service UUID：

```text
19B10010-E8F2-537E-4F6C-D104768A1214
```

Characteristics：

| Characteristic | UUID | 权限 | Phase 4 行为 |
| --- | --- | --- | --- |
| OTA Control | `19B10011-E8F2-537E-4F6C-D104768A1214` | READ + WRITE | 接收命令并记录，不执行 OTA |
| OTA Data | `19B10012-E8F2-537E-4F6C-D104768A1214` | READ + NOTIFY | 状态占位为 IDLE |

真实 Wi-Fi / HTTPS OTA 在 Phase 10 实现。

## 广播

保持原 Arduino 设计意图：

- 广播 Omi Main Service UUID。
- Device Name 为 `OMI Glass`。
- Device Name 放入 Scan Response。
- Connectable undirected advertising。
- General discoverable。

广播间隔：

```text
Min 0x0140 = 200ms
Max 0x0280 = 400ms
```

## 连接参数

连接建立后请求：

```text
Min interval = 20 × 1.25ms = 25ms
Max interval = 40 × 1.25ms = 50ms
Latency      = 0
Timeout      = 800 × 10ms = 8s
```

这些参数与 Arduino `config.h` 保持一致。

## CCCD / Subscribe

NimBLE 对带 `NOTIFY` 标志的 Characteristic 自动管理 CCCD。

Phase 4 通过 `BLE_GAP_EVENT_SUBSCRIBE` 跟踪：

```text
Audio Data
Photo Data
Battery Level
OTA Data
```

其中最重要的是：

```text
ble_service_audio_subscribed()
```

Phase 5 会使用这个状态决定是否发送实时 Opus 帧。

## MTU

Preferred ATT MTU：

```text
517
```

连接后通过 `BLE_GAP_EVENT_MTU` 保存实际协商值。

实际 MTU 由本机和手机端两侧能力共同决定，因此真机不能假设一定等于 517。

## 与主循环关系

NimBLE Host 运行于独立 FreeRTOS Task：

```text
NimBLE Host Task
    ├─ GAP event
    ├─ GATT access
    └─ subscribe / MTU
```

原有业务主循环仍负责：

```text
board
mic
opus
```

主循环只读取：

```text
ble_service_is_connected()
```

再调用：

```text
board_service_set_connected(...)
```

从而恢复原 Arduino 固件的 LED 连接状态表现。

## Phase 4 明确没有做什么

### 没有发送实时音频

当前：

```text
Mic
 ↓
PCM
 ↓
Opus
 ↓
on_opus_encoded()
 ↓
只做统计
```

Phase 5 才改为：

```text
Opus
 ↓
Audio TX Ring Buffer
 ↓
Packet Header
 ↓
BLE Notify
```

### 没有真正拍照

Photo Control 写入只记录命令。

Camera 在 Phase 6 迁移。

### 没有真正 Battery ADC

Battery Level 当前只是 GATT 结构占位。

ADC 在 Phase 8 迁移。

### 没有真正 OTA

OTA Control 只接收并记录命令。

Wi-Fi / HTTPS OTA 在 Phase 10 迁移。

## GitHub Actions 编译验证

新增：

```text
.github/workflows/omiglass-esp-idf.yml
```

使用官方镜像：

```text
espressif/idf:v6.1
```

执行：

```bash
cd omiGlass/esp32
idf.py set-target esp32s3
idf.py build
```

因此后续迁移可以在 GitHub 上持续验证 ESP-IDF v6.1 编译兼容性。

## Phase 4 真机验证清单

1. `idf.py build` 成功。
2. XIAO ESP32S3 能启动。
3. 手机可以扫描到 `OMI Glass`。
4. 广播中存在 Omi Main Service UUID。
5. 手机可以连接。
6. 连接后 LED 状态符合预期。
7. GATT 中存在 4 个目标业务 Service。
8. 所有 Characteristic UUID 和权限与 Arduino 版本一致。
9. Audio Codec 读取值为 `21`。
10. Manufacturer / Model / Firmware / Hardware / Serial 可正确读取。
11. Audio Data 打开通知后日志显示订阅开启。
12. 关闭通知后日志显示订阅关闭。
13. MTU 交换日志正常。
14. 断开连接后自动重新广播。
15. Phase 4 全程没有实时 Opus notify。

## 下一阶段

Phase 5：BLE Audio Uplink。

只在 Phase 4 的连接、订阅、MTU 和 GATT 结构验证稳定后，才把：

```text
on_opus_encoded()
```

接入：

```text
Audio TX Ring Buffer
→ 2-byte packet index
→ 1-byte sub-index
→ Opus payload
→ NimBLE notify
```

手机端数据包格式保持不变。
