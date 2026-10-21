# Phase 4：ESP-IDF Native NimBLE / GATT

本阶段目标：**只迁移 BLE Core、广播、连接和 GATT 结构，不发送实时音频。**

ESP-IDF 基线：`v6.1.x`

## 1. 迁移范围

已从 NimBLE-Arduino 迁移到 ESP-IDF Native NimBLE：

- NimBLE Host 初始化
- GAP Device Name
- 广播 / Scan Response
- Connect / Disconnect
- ATT MTU
- Connection Parameters
- GATT Server
- CCCD / Notify 订阅状态
- Omi Main Service
- Battery Service
- Device Information Service
- OTA Service

Phase 4 不实现：

- Opus 实时 BLE notify（Phase 5）
- Camera 数据（Phase 6 / 7）
- Battery ADC（Phase 8）
- OTA 下载/安装（Phase 10）

## 2. GATT 对照表

### Omi Main Service

Service UUID：

`19B10000-E8F2-537E-4F6C-D104768A1214`

| Characteristic | UUID | 权限 | Phase 4 行为 |
|---|---|---|---|
| Audio Data | `19B10001-E8F2-537E-4F6C-D104768A1214` | Read + Notify | 可发现/可订阅；暂不发送音频 |
| Audio Codec | `19B10002-E8F2-537E-4F6C-D104768A1214` | Read | 返回 Codec ID `21` |
| Photo Data | `19B10005-E8F2-537E-4F6C-D104768A1214` | Read + Notify | 可发现/可订阅；暂不发送图片 |
| Photo Control | `19B10006-E8F2-537E-4F6C-D104768A1214` | Write | 接收并记录命令；暂不驱动 Camera |

### Battery Service

Service UUID：`0x180F`

| Characteristic | UUID | 权限 | Phase 4 行为 |
|---|---|---|---|
| Battery Level | `0x2A19` | Read + Notify | 当前返回占位值 0；Phase 8 接真实 ADC |

### Device Information Service

Service UUID：`0x180A`

| Characteristic | UUID | 内容 |
|---|---|---|
| Manufacturer Name | `0x2A29` | `Based Hardware` |
| Model Number | `0x2A24` | `OMI Glass` |
| Firmware Revision | `0x2A26` | 当前固件版本 |
| Hardware Revision | `0x2A27` | `ESP32-S3-v1.0` |
| Serial Number | `0x2A25` | 与 Arduino 版本兼容的 eFuse MAC 派生序列号 |

### OTA Service

Service UUID：

`19B10010-E8F2-537E-4F6C-D104768A1214`

| Characteristic | UUID | 权限 | Phase 4 行为 |
|---|---|---|---|
| OTA Control | `19B10011-E8F2-537E-4F6C-D104768A1214` | Read + Write | 可读 IDLE 状态；写入只记录命令 |
| OTA Data | `19B10012-E8F2-537E-4F6C-D104768A1214` | Read + Notify | 可发现/可订阅；暂不发送 OTA 进度 |

## 3. 广播策略

保持原 Arduino 设计意图：

- Advertising Data：放 Omi Main Service 的 128-bit UUID
- Scan Response：放完整设备名 `OMI Glass`
- Connectable：Undirected Connectable
- Discoverable：General Discoverable
- 广播间隔：200ms ~ 400ms

这样可以避免完整设备名和 128-bit Service UUID 同时挤进传统 31-byte Advertising Payload。

## 4. 连接参数

保持原固件：

- Preferred MTU：517
- Connection interval：25ms ~ 50ms
- Slave latency：0
- Supervision timeout：8s
- 最大连接数：1

## 5. 订阅状态

通过 `BLE_GAP_EVENT_SUBSCRIBE` 跟踪：

- Audio Data Notify
- Photo Data Notify
- Battery Level Notify
- OTA Data Notify

Phase 4 只记录订阅状态，不主动发送媒体数据。

## 6. app_main 当前顺序

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
   └── 主循环
          ├── 同步 BLE connected -> LED
          ├── Mic PCM
          ├── Opus encode
          └── 音频统计（不发送 BLE）
```

BLE Host 运行在独立 FreeRTOS Task，业务主循环不轮询 BLE 栈。

## 7. 真机验证清单

使用现有 Omi App 或 nRF Connect 验证：

1. 手机能扫描到 `OMI Glass`。
2. 广播中能识别 Omi Main Service UUID。
3. 能成功连接。
4. GATT 中存在 Omi Main / Battery / Device Info / OTA 四组业务服务。
5. Audio Codec 读取值为 `0x15`（十进制 21）。
6. Audio Data 可打开 Notify，串口输出订阅开启日志。
7. Photo Data / Battery / OTA Data 可订阅。
8. Photo Control 写 1 byte 后串口能看到命令值。
9. OTA Control 写命令后串口能看到命令码，但不会真正执行 OTA。
10. Device Information 各字段可正常读取。
11. MTU 协商日志正常；目标最高 517。
12. 断开后设备自动重新广播。
13. BLE 连接后 GPIO21 LED 进入“已连接”状态。
14. Mic / Opus 仍持续运行，但没有 Audio Data notify 流量。

## 8. Phase 5 接口边界

Phase 5 才会将：

```text
on_opus_encoded()
   ↓
Audio TX Ring Buffer
   ↓
packet header
   ↓
BLE Audio Data notify
```

接入当前 GATT Server。

原协议保持：

```text
byte 0-1：packet index（little-endian）
byte 2：sub-index
byte 3...：Opus payload
```
