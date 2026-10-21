# OmiGlass ESP-IDF 固件迁移

本目录用于将现有 `../firmware/` 中的 Arduino/PlatformIO 固件逐步迁移为原生 ESP-IDF 实现。

## 固定开发基线

- 硬件：Seeed Studio XIAO ESP32S3 Sense
- ESP-IDF：**v6.1.x**
- Target：`esp32s3`
- Flash：8MB
- PSRAM：Octal PSRAM
- 原 Arduino 固件：继续保留为功能与协议参考，不在迁移过程中直接改写

`main/idf_component.yml` 已将 ESP-IDF 版本约束固定为：

```yaml
dependencies:
  idf:
    version: ">=6.1,<6.2"
```

这样后续不会因为误用 v5.x 或未来 v6.2+ API 导致迁移基线漂移。

## 当前外部组件

Phase 3 开始使用 ESP-IDF Component Manager：

```yaml
78/esp-opus: "1.0.5"
```

用于替代原 PlatformIO 中的 Arduino Opus 依赖，同时继续使用标准 `opus.h` / `opus_encode()` API。

首次构建时 Component Manager 会根据 `main/idf_component.yml` 下载依赖并生成 `dependencies.lock`。`dependencies.lock` 应由 `idf.py` 自动生成，不手工编写。

## 迁移原则

- 保持 `omiGlass/firmware/` 不变，直到 ESP-IDF 实现达到功能等价。
- 除非明确记录协议变更，否则保持现有手机端 BLE UUID、数据包格式和 Codec ID 不变。
- 优先使用 ESP-IDF 原生驱动和组件，不使用 Arduino-as-component 作为过渡捷径。
- 每个阶段先实现功能等价，再进入下一阶段。
- BLE 时序、实时音频、照片传输和功耗属于高风险兼容区域，需要单独验证。

## 当前已迁移链路

```text
app_main
   ↓
board_service
   ↓
PDM microphone / ESP-IDF I2S PDM RX
   ↓
PCM16 mono / 16kHz
   ↓
opus_service
   ↓
Opus 20ms frame / Codec ID 21
```

BLE、Camera、Battery、Power 和 OTA 会按 `MIGRATION_PLAN.md` 继续迁移。

## 构建

请使用 ESP-IDF v6.1.x 环境：

```bash
cd omiGlass/esp32
idf.py set-target esp32s3
idf.py build
```

烧录并查看日志：

```bash
idf.py -p <PORT> flash monitor
```

当前聊天模式可以直接维护 GitHub 代码，但不能替代本地 ESP-IDF 编译和 XIAO ESP32S3 真机验证，因此仓库进度文件会始终区分“代码迁移完成”和“真机验证完成”。
