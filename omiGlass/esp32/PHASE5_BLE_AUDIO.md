# Phase 5：BLE 实时音频发送

## 目标

本阶段把 Phase 3 已经生成的 Opus 帧接到 Phase 4 已经建立的 Audio Data Characteristic，通过 ESP-IDF v6.1 原生 NimBLE 发送实时通知。

数据链：

```text
PDM Mic
  ↓
PCM16 / 16kHz / Mono
  ↓
Opus Encoder
  ↓
20ms Opus Frame
  ↓
audio_tx_service
  ↓
Omi 3-byte Header
  ↓
ble_gatts_notify_custom()
  ↓
手机 App
```

## 1. 手机端协议保持不变

每一个 BLE Audio Data Notification：

```text
byte 0-1：packet index，little-endian
byte 2  ：sub-index
byte 3+ ：Opus payload
```

当前 sub-index 固定为：

```text
0
```

因为当前一帧 Opus 最大 160 bytes，在 MTU 517 下可以一次通知完整发送，不需要二次分片。

最大业务包：

```text
3-byte header + 160-byte Opus = 163 bytes
```

ATT Notification 可承载的 Attribute Value 最大长度为：

```text
MTU - 3
```

因此完整 163-byte 音频包至少需要：

```text
MTU >= 166
```

项目 Preferred MTU 仍为 517。

## 2. 新增 audio_tx_service

新增：

- `main/audio_tx_service.h`
- `main/audio_tx_service.cpp`

职责：

- 接收 Opus encoded frame。
- 保存到 16 帧实时发送队列。
- 添加 packet index / sub-index。
- 控制每轮最大发送数量。
- 调用 `ble_service_notify_audio()`。
- 统计发送、丢帧、MTU 不足和 notify 错误。

BLE GATT 本身仍由 `ble_service` 负责。

## 3. 发送队列

与原 Arduino 固件保持相同的 16 帧容量：

```text
OMI_AUDIO_TX_QUEUE_FRAMES = 16
```

20ms 一帧时，理论缓冲上限：

```text
16 × 20ms = 320ms
```

不过本实现不会主动追求把 320ms 队列填满。

### 队列满时的策略

如果发送速度跟不上编码速度：

```text
丢最旧帧
保留最新帧
```

原因：实时语音最重要的是低延迟。如果保留旧帧，延迟会越来越大，最后虽然“没有丢包”，但用户听到的是明显滞后的声音。

## 4. 未连接 / 未订阅时不缓存旧语音

如果：

```text
BLE 未连接
或
Audio Data Notify 未订阅
```

新的 Opus 帧不会进入发送队列。

这和简单地一直缓存相比有一个重要优势：

客户端刚打开 Notify 时，收到的是当前声音，而不是几百毫秒前的历史声音。

## 5. 发送节奏

每次 `audio_tx_service_process()` 最多发送：

```text
2 帧
```

通常主循环每约 20ms 产生一帧，因此正常状态下一轮发送一帧即可。

允许最多两帧的目的，是在主循环偶尔被其他工作拖慢时，可以轻微追赶积压，但不会一次 while 循环把大量 BLE 包全部塞进 Host 队列。

这比原 Arduino 版本“while 有数据就一直发”的方式更保守，更容易控制拥塞。

## 6. packet index

`packet index` 为 16-bit：

```text
0
1
2
...
65535
0
...
```

当前只有 `ble_gatts_notify_custom()` 成功进入 NimBLE 发送路径后才递增 packet index。

## 7. BLE 层新增能力

`ble_service` 在 Phase 5 新增：

```cpp
esp_err_t ble_service_notify_audio(const uint8_t *packet, size_t len);
```

BLE Core 现在额外保存：

```text
当前 connection handle
Audio Data value handle
当前协商 MTU
Audio Data Notify 订阅状态
```

发送前必须同时满足：

```text
NimBLE ready
BLE connected
Audio Data subscribed
connection handle valid
Audio Data handle valid
packet <= MTU - 3
```

## 8. NimBLE mbuf 生命周期

发送路径：

```cpp
ble_hs_mbuf_from_flat(...)
ble_gatts_notify_custom(...)
```

NimBLE 的 `ble_gatts_notify_custom()` 会无论成功还是失败都消费传入的 mbuf。

因此代码中调用完成后不会再次手工执行：

```cpp
os_mbuf_free_chain()
```

否则可能造成 double free。

## 9. 当前统计日志

每约一秒输出：

```text
麦克风：samples / peak
Opus：frames / bytes / avg / max
BLE Audio：
  是否订阅
  当前 MTU
  入队帧数
  发送帧数
  未连接/未订阅跳过帧数
  队列满丢帧
  MTU 不足丢帧
  notify 错误
  当前队列深度
```

稳定状态目标大致为：

```text
PCM samples       ≈ 16000 / 秒
Opus frames        ≈ 50 / 秒
BLE sent frames    ≈ 50 / 秒
queue depth        接近 0
queue full drops   = 0
MTU drops          = 0
notify errors      = 0
```

## 10. 真机验证清单

1. 手机能连接 `OMI Glass`。
2. Audio Codec 仍读取到 `21 / 0x15`。
3. 手机订阅 Audio Data Notify。
4. 串口显示 Audio Data Notify 已开启。
5. MTU 协商到足够承载当前 Opus 包的大小，目标 517。
6. BLE Audio 每秒发送约 50 帧。
7. 手机端能连续收到 3-byte header + Opus payload。
8. packet index 连续递增并按 little-endian 解析。
9. sub-index 始终为 0。
10. 手机端能使用原有解码逻辑播放/处理 Opus，不需要修改协议。
11. 正常连接下队列深度长期接近 0。
12. 没有持续 queue full drop。
13. 没有持续 notify error。
14. 关闭 Audio Data Notify 后立即停止积压音频。
15. 再次打开 Notify 后从当前实时声音开始发送，不补发旧音频。
16. 断开 BLE 后发送队列清空。
17. 重连并重新订阅后恢复实时发送。

## 11. Phase 5 暂时不处理的内容

- Camera / Photo BLE：Phase 6 / 7。
- Battery ADC：Phase 8。
- Light Sleep / Deep Sleep：Phase 9。
- Wi-Fi / HTTPS OTA：Phase 10。
- 多任务架构重构：Phase 12。

## 12. 后续与照片传输的优先级

原 Arduino 固件规定：

```text
Audio > Photo
```

Phase 7 接入 Photo BLE 后仍需保持：

```text
如果 Audio TX Queue 有数据
    优先发送音频
    照片分片让路
```

因此当前 `audio_tx_service` 会作为以后媒体发送调度的高优先级数据源。
