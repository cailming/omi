# Phase 7：Photo BLE

## 目标

在 Phase 6 Camera 和 Phase 4 Native NimBLE 的基础上，迁移原 Arduino 固件的图片 BLE 分包传输协议。

本阶段保持手机端协议不变，并明确执行：

> Audio 优先于 Photo。

ESP-IDF 基线固定为 `v6.1.x`。

---

## 1. 数据链路

```text
Photo Control
    ↓
photo_service
    ↓
camera_service_capture()
    ↓
VGA JPEG framebuffer（PSRAM）
    ↓
Photo 分包状态机
    ↓
ble_service_notify_photo()
    ↓
Photo Data Characteristic Notify
    ↓
手机
```

上传结束后：

```text
0xFFFF End Marker
    ↓
camera_service_release_frame()
```

因此 Camera framebuffer 会一直保留到整张图片发送完成，不会在图片尚未发送时被归还。

---

## 2. 原 Arduino 图片协议

原实现的 Photo Data 包不是按 MTU 动态切片，而是固定使用约 200 字节 JPEG 数据。

### 首包

```text
byte 0   frame index low = 0
byte 1   frame index high = 0
byte 2   orientation
byte 3+  JPEG data，最多 199 bytes
```

首包最大长度：

```text
2 + 1 + 199 = 202 bytes
```

### 后续包

```text
byte 0   frame index low
byte 1   frame index high
byte 2+  JPEG data，最多 200 bytes
```

后续包最大长度：

```text
2 + 200 = 202 bytes
```

frame index 为 little-endian。

### 结束标记

JPEG 数据全部发完后，再单独发送：

```text
FF FF
```

即：

```text
frame index = 0xFFFF
```

该协议在 ESP-IDF 版本中保持不变。

---

## 3. MTU 要求

最大 Photo Data notification value 为 202 bytes。

ATT Notification 本身还需要 3 bytes ATT Header，因此：

```text
最低 ATT MTU = 202 + 3 = 205
```

项目 Preferred MTU 仍为：

```text
517
```

与实时 Audio 不同：

- Audio：MTU 不够时丢弃旧音频帧，保证实时性。
- Photo：MTU 不够时暂停上传，等待协商完成，保证图片完整性。

---

## 4. Audio > Photo

原 Arduino 固件在图片发送前检查音频发送缓冲区，只要 Audio 有数据就让 Photo 暂停。

ESP-IDF 版保持同样原则，并进一步做成显式接口：

```text
audio_tx_service_has_pending()
```

主循环顺序：

```text
Mic
 ↓
Opus
 ↓
Audio TX process
 ↓
Photo process
```

Photo process 内部再次判断：

```text
if Audio Queue 有待发帧:
    本轮不发送 Photo
```

每轮 Photo 最多发送：

```text
2 chunks
```

与原 Arduino 固件一致。

---

## 5. Notify 失败策略

Audio 与 Photo 的策略不同。

### Audio

实时性优先：

```text
notify 失败
   ↓
丢掉旧帧
   ↓
继续最新音频
```

### Photo

完整性优先：

```text
notify 失败
   ↓
不移动 sent_bytes / chunk_index
   ↓
下一轮重试同一个 chunk
```

这样不会因为瞬时 BLE 拥塞造成 JPEG 中间缺块。

---

## 6. Photo Control

Photo Control Characteristic 仍为：

```text
19B10006-E8F2-537E-4F6C-D104768A1214
WRITE
1 byte
```

BLE Host Task 收到命令后不会直接操作 Camera，而是保存成原子命令：

```text
NimBLE Host Task
   ↓
保存 Photo Control
   ↓
app_main / photo_service 取走
   ↓
操作 Camera
```

避免在 BLE Host Task 中执行 Camera capture。

### 命令语义

按原 Arduino `int8_t` 行为：

```text
-1 / 0xFF   单拍
0           停止后续拍照
5..127      开启间隔拍照
```

原 Arduino 代码虽然条件写到 `<=300`，但 Characteristic 只有 1 byte 且变量类型为 `int8_t`，因此迁移阶段不扩展线协议。

### 间隔

和原 Arduino 固件一致：

客户端给出正值后，实际仍使用项目固定配置：

```text
30 秒
```

而不是直接把命令值当作秒数。

---

## 7. 默认行为

启动后默认：

```text
自动拍照开启
间隔 = 30 秒
首次连接 + Photo Data 已订阅后立即拍一张
```

相比原 Arduino 仅判断“connected”，ESP-IDF 版增加了 `Photo Data 已订阅` 条件。

原因：如果客户端没有订阅 Photo Data，提前占用唯一 Camera framebuffer 没有意义。

这不改变手机端数据协议，只减少无效抓图和 framebuffer 占用。

---

## 8. 断连 / 取消订阅

如果上传过程中：

```text
BLE disconnect
或
Photo Data unsubscribe
```

则：

```text
取消当前 upload
    ↓
归还 framebuffer
    ↓
清空图片发送状态
```

防止单 framebuffer 被长期占用。

---

## 9. 新增接口

### `ble_service`

新增：

```cpp
esp_err_t ble_service_notify_photo(const uint8_t *data, size_t len);
bool ble_service_take_photo_control(int8_t *value);
```

### `audio_tx_service`

新增：

```cpp
bool audio_tx_service_has_pending();
```

### `photo_service`

新增：

```text
main/photo_service.h
main/photo_service.cpp
```

负责：

- Photo Control
- 拍照调度
- framebuffer 生命周期
- JPEG chunking
- Audio 优先级
- MTU 等待
- Photo Data notify
- End Marker
- 传输统计

---

## 10. 当前主循环

```text
while(true)
   │
   ├── Board / LED
   │
   ├── Mic PCM
   │
   ├── Opus Encode
   │
   ├── Audio BLE TX
   │
   ├── Photo Service
   │      ├── Photo Control
   │      ├── Camera Capture
   │      ├── Audio pending ? yield
   │      ├── max 2 chunks
   │      └── FF FF + release framebuffer
   │
   └── Runtime statistics
```

---

## 11. 真机验证清单

1. 手机连接 `OMI Glass`。
2. 手机订阅 Photo Data Notify。
3. 首次订阅后能收到一张完整 JPEG。
4. 首包 frame index 为 `0x0000`。
5. 首包 byte 2 为 orientation `2`。
6. 首包最多携带 199 bytes JPEG。
7. 后续包最多携带 200 bytes JPEG。
8. frame index little-endian 且连续递增。
9. JPEG 最后收到单独 `FF FF`。
10. 手机端可以重新拼装成有效 VGA JPEG。
11. `0xFF` Photo Control 可以触发单拍。
12. `0x00` 可以停止后续周期拍照。
13. 正值命令可以重新启动周期拍照。
14. 音频和图片同时发送时，音频无明显卡顿。
15. Audio Queue 非空时 Photo 统计出现 `yielded_to_audio`。
16. Photo 上传期间 framebuffer 一直被持有，结束后正常释放。
17. BLE 断开时当前上传自动取消并释放 framebuffer。
18. 长时间周期拍照没有 framebuffer 泄漏。

---

## 12. 当前验证边界

当前代码已经写入 GitHub，但仍未在 XIAO ESP32S3 Sense 真机上完成：

```text
ESP-IDF v6.1 build
BLE 手机连接
JPEG 重组
Audio + Photo 并发压力测试
```

因此当前准确状态是：

> Phase 7 代码迁移完成，待编译和真机协议验证。
