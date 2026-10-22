# Phase 6：Camera 原生 ESP-IDF 迁移

## 目标

本阶段只迁移 Camera 初始化、JPEG framebuffer 获取和释放，不发送照片 BLE。

数据链：

```text
XIAO ESP32S3 Sense Camera
  ↓
esp32-camera
  ↓
VGA JPEG framebuffer
  ↓
CameraFrameView
  ↓
日志验证
  ↓
esp_camera_fb_return()
```

Photo Data BLE Transport 留到 Phase 7。

## 1. 组件依赖

`main/idf_component.yml` 新增：

```yaml
espressif/esp32-camera:
  version: "2.1.8"
```

当前迁移基线继续固定：

```text
ESP-IDF >=6.1,<6.2
ESP32-S3
```

## 2. Camera 引脚

保持原 Arduino 固件的 XIAO ESP32S3 Sense 映射：

```text
PWDN   -1
RESET  -1
XCLK   GPIO10
SIOD   GPIO40
SIOC   GPIO39
Y9     GPIO48
Y8     GPIO11
Y7     GPIO12
Y6     GPIO14
Y5     GPIO16
Y4     GPIO18
Y3     GPIO17
Y2     GPIO15
VSYNC  GPIO38
HREF   GPIO47
PCLK   GPIO13
```

新版 `esp32-camera` 配置使用：

```text
pin_sccb_sda
pin_sccb_scl
```

不再主动使用旧的 `pin_sscb_*` 兼容字段名。

## 3. Camera 参数

保持原固件：

```text
Pixel Format      JPEG
Frame Size        VGA 640x480
JPEG Quality      25
XCLK              6MHz
Framebuffer Count 1
Framebuffer        PSRAM
Grab Mode          CAMERA_GRAB_LATEST
```

方向元数据：

```text
180°
orientation = 2
```

这里的 orientation 仍然是业务元数据，并没有在 MCU 上重新编码/旋转 JPEG。

## 4. 新增 camera_service

新增：

- `main/camera_service.h`
- `main/camera_service.cpp`

主要接口：

```cpp
esp_err_t camera_service_init();
bool camera_service_is_ready();
esp_err_t camera_service_capture(CameraFrameView *view);
void camera_service_release_frame();
void camera_service_deinit();
```

`CameraFrameView` 提供：

```text
data
len
width
height
orientation
```

其中 `data` 只在 `camera_service_release_frame()` 之前有效。

## 5. framebuffer 生命周期

遵循 `esp32-camera` 标准用法：

```text
esp_camera_fb_get()
  ↓
使用 framebuffer
  ↓
esp_camera_fb_return()
```

每次新拍照之前，如果上一帧仍然被持有，会先归还上一帧。

这样避免单 framebuffer 配置下发生资源长期占用。

## 6. 当前 app_main 行为

保持原 Arduino 固件的默认行为意图：

```text
BLE 已连接
  ↓
默认每 30 秒尝试拍一张
```

原 Arduino 固件初始化时会：

```text
lastCaptureTime = now - captureInterval
```

因此第一次建立 BLE 连接后可以立即进入第一次拍照。

ESP-IDF 版也保留这个行为。

但 Phase 6 拍完之后只做：

```text
打印 JPEG 大小
打印 width / height
打印 orientation
立即 release framebuffer
```

明确不做：

```text
Photo Data notify
照片分包
frame index
end marker
```

这些属于 Phase 7。

## 7. 与实时音频的优先级

主循环顺序保持：

```text
Mic
 ↓
Opus
 ↓
BLE Audio TX
 ↓
Camera
```

也就是说 Camera 放在音频发送之后。

这是为了继续保持原固件：

```text
Audio > Photo
```

的实时性原则。

## 8. 真机日志预期

连接手机后第一次拍照成功时应看到类似：

```text
Camera 初始化成功
Phase 6 Camera 已启动
拍照成功：640x480，JPEG=xxxxx bytes，orientation=2(180°)
Phase 6 Camera 验证：JPEG=xxxxx bytes，640x480，方向=2
```

JPEG 大小会随现场画面复杂度变化，不要求固定值。

## 9. 真机验证清单

1. `esp_camera_init()` 成功。
2. 识别到实际 Camera sensor。
3. framebuffer 位于可用 PSRAM 环境。
4. `esp_camera_fb_get()` 稳定返回非空。
5. width / height 为 640x480。
6. format 为 JPEG。
7. JPEG 长度为合理非零值。
8. orientation 为 2（180°）。
9. framebuffer 使用后能正常归还。
10. 连续多次 30 秒周期抓图不出现内存泄漏。
11. Camera 抓图期间 BLE 不掉线。
12. Camera 抓图期间 Mic / Opus 不永久停止。
13. BLE Audio Queue 不持续堆积或爆满。
14. 当前阶段手机端没有收到 Photo Data notify，符合预期。

## 10. Phase 7 接口边界

Phase 7 会直接复用：

```cpp
CameraFrameView
```

然后实现：

```text
JPEG framebuffer
  ↓
Photo BLE chunking
  ↓
frame index
  ↓
orientation byte
  ↓
Photo Data notify
  ↓
0xFFFF end marker
```

并保持音频高优先级：

```text
Audio TX Queue 有数据时
Photo BLE 主动让路
```
