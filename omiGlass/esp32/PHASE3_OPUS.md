# Phase 3：Opus 编码器迁移记录

## 状态

代码迁移完成，等待 ESP-IDF v6.1 本地编译与真机验证。

## 固定版本

- ESP-IDF：`>=6.1,<6.2`
- Opus Component：`78/esp-opus 1.0.5`
- Target：ESP32-S3

版本约束位于：

`main/idf_component.yml`

## 原 Arduino 实现

原固件通过 PlatformIO 引入 Arduino Opus 库，但业务代码实际使用的是标准 Opus C API：

- `opus_encoder_create()`
- `opus_encoder_ctl()`
- `opus_encode()`
- `opus_encoder_destroy()`

因此 Phase 3 不重写音频编码协议，只替换构建依赖和外围服务封装。

## ESP-IDF 实现

新增：

- `main/opus_service.h`
- `main/opus_service.cpp`

数据链路：

```text
PDM Mic
  ↓
PCM16 / 16kHz / mono
  ↓
opus_service_receive_pcm()
  ↓
8000 samples PCM ring buffer
  ↓
每满 320 samples
  ↓
opus_encode()
  ↓
encoded callback
```

## 与原固件保持一致的参数

- Sample Rate：16000 Hz
- Channels：1
- Application：`OPUS_APPLICATION_VOIP`
- Frame：320 samples / 20ms
- Bitrate：32000 bps
- Complexity：3
- Signal：Voice
- VBR：开启
- VBR Constraint：关闭
- LSB Depth：16
- DTX：关闭
- In-band FEC：关闭
- Packet Loss Percentage：0
- 最大输出：160 bytes
- Omi Codec ID：21

## PCM 环形缓冲策略

保留原 Arduino 行为：

- PCM 输入允许一次提交任意数量 samples。
- 环形缓冲容量为 8000 samples，约 500ms。
- 每凑满 320 samples 编码一帧。
- 如果环形缓冲满，不阻塞麦克风实时链路，而是丢弃最旧 sample。

虽然 Phase 2 当前一次通常读取 320 samples，但 Opus 层并不依赖这个事实，从而保持模块解耦。

## 内存策略

- PCM ring buffer 优先 PSRAM。
- 320-sample Opus input buffer 优先 PSRAM。
- 160-byte Opus output buffer 优先 PSRAM。
- PSRAM 分配失败时回退普通 8-bit 可访问 RAM。

## 当前 app_main 验证逻辑

在 BLE 尚未迁移之前，编码完成后的 Opus 数据暂时不发送，只统计：

- 最近约 1 秒编码帧数。
- 最近约 1 秒编码总字节数。
- 平均每帧字节数。
- 最大帧字节数。

理论目标：

```text
16000 samples/s ÷ 320 samples/frame = 50 frames/s
```

真机正常运行时，日志应大致接近：

```text
麦克风：最近约 1 秒收到 16000 samples，峰值=...
Opus：最近约 1 秒编码 50 帧，共 ... 字节，平均 ... 字节/帧，最大 ... 字节
```

由于使用 VBR，帧长会随语音内容变化，但任何帧都不应超过当前协议缓冲上限 160 bytes。

## 尚未验证

当前聊天模式没有本地 ESP-IDF 编译器与实体 XIAO ESP32S3，因此以下项目仍需本地验证：

1. ESP-IDF v6.1 能成功解析 `78/esp-opus 1.0.5`。
2. `idf.py build` 成功。
3. Opus 组件与 ESP-IDF v6.1 编译器实际兼容。
4. 每秒 PCM 约 16000 samples。
5. 每秒 Opus 约 50 frames。
6. 单帧不超过 160 bytes。
7. 连续运行无 heap 泄漏或编码错误。
8. 后续接入手机 BLE 后，现有 App 可直接解码，无协议修改。

## 下一阶段接口

Phase 4/5 BLE 迁移时不再修改 Opus 编码器核心逻辑，只需要把：

```cpp
on_opus_encoded(const uint8_t *data, size_t len)
```

接到 BLE Audio TX ring/queue，并继续使用原 Omi 音频包头：

```text
byte 0-1：packet index，小端
byte 2：sub-index
byte 3...：Opus payload
```
