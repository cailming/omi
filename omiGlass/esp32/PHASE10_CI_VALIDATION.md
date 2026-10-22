# Phase 10：ESP-IDF v6.1 CI 验证结果

验证日期：2026-10-22

分支：`feature/omiglass-esp-idf-migration`

## 结论

Phase 10 合入后的 Native ESP-IDF 工程已经在官方：

```text
espressif/idf:v6.1
```

环境中真实执行：

```bash
idf.py set-target esp32s3
idf.py build
```

并成功完成编译。

因此从 Phase 10 开始，可以把验证边界从：

> 代码迁移完成，但尚未实际编译

提升为：

> **Phase 0～10 当前源码已通过 ESP-IDF v6.1 CI 编译。**

这仍不等于真机烧录、BLE 手机联调、Camera/Mic 硬件验证和真实 HTTPS OTA 已通过。

---

## 第一轮真实编译发现的问题

CI 闭环首次真正进入 `idf.py build` 后发现两个 v6.1 编译问题。

### 1. NimBLE Utility Header

错误：

```text
ble_service.cpp:541:14:
error: 'ble_hs_util_ensure_addr' was not declared in this scope
```

原因：

ESP-IDF v6.1 / NimBLE 中：

```cpp
ble_hs_util_ensure_addr()
```

声明位于：

```cpp
#include "host/util/util.h"
```

不能依赖 `host/ble_hs.h` 间接包含。

修复：显式包含 NimBLE Utility Header。

### 2. uint32_t printf Format

错误：

```text
board_service.cpp:52:17:
error: format '%X' expects argument of type 'unsigned int',
but argument has type 'uint32_t' {aka 'long unsigned int'}
[-Werror=format=]
```

原写法：

```cpp
"%04X%08X"
```

修复后使用 `<cinttypes>`：

```cpp
PRIX32
```

避免假定 `uint32_t` 在所有 ESP-IDF 工具链中都等于 `unsigned int`。

修复后下一轮完整 `idf.py build` 成功。

---

## 固件实际大小

CI 成功构建生成：

```text
omiGlass/esp32/build/omiglass_esp_idf.bin
```

实际大小：

```text
1,560,080 bytes
```

当前 OTA App Slot：

```text
0x1C0000 = 1,835,008 bytes
```

剩余：

```text
274,928 bytes
```

当前使用率：

```text
85%
```

---

## 分区结论

当前：

```text
firmware    1,560,080
slot        1,835,008
margin        274,928
usage             85%
```

因此 Phase 10 暂时**不修改**从 Arduino 固件继承的双 OTA 分区大小。

原因：

1. 当前固件仍可完整放入 app0/app1；
2. 保持原分区布局有利于迁移阶段兼容；
3. 分区表不是普通 App OTA 能安全随意改变的内容；
4. 在没有必要时不应为了使用完整 8MB Flash 主动改变现有部署布局。

但 85% 已属于需要持续监控的水平。

Phase 11/12 每次加入明显功能后，CI 都需要继续检查：

```text
firmware size
OTA slot margin
usage percent
```

如果固件继续逼近 `0x1C0000`，再单独设计新的 8MB 分区布局和升级迁移方案，而不是简单改 CSV。

---

## CI 工作流增强

当前 `.github/workflows/omiglass-esp-idf.yml` 已支持：

### 1. 固定 ESP-IDF

```text
espressif/idf:v6.1
```

### 2. 显式加载 ESP-IDF 环境

```bash
source "$IDF_PATH/export.sh"
```

原因：GitHub Actions job container 会覆盖官方 Docker 镜像默认 entrypoint。

### 3. 编译错误 Annotation

失败时自动抽取：

```text
fatal error:
error:
CMake Error
FAILED:
undefined reference
partition overflow
```

并转换为 GitHub Annotation。

这样 Chat 模式可以直接读取真实编译器错误继续修复。

### 4. 固件大小 Notice

成功后自动报告：

```text
firmware bytes
OTA slot bytes
margin bytes
usage percent
```

### 5. 取消过期构建

工作流使用同一分支 concurrency group：

```text
cancel-in-progress: true
```

连续迁移提交时只保留最新构建，避免旧 commit 反复拉取 ESP-IDF 大镜像并占用 runner。

---

## 当前仍需真机验证

虽然 CI 已通过，但以下内容仍必须在 XIAO ESP32S3 Sense 上验证：

- Boot / PSRAM；
- GPIO1 Button；
- GPIO21 LED；
- PDM Mic；
- Opus 实时编码；
- BLE GATT；
- BLE Audio；
- Camera；
- Photo BLE；
- Battery ADC 与万用表误差；
- DFS / Automatic Light Sleep；
- Deep Sleep / GPIO1 Wake；
- Wi-Fi + BLE coexistence；
- TLS Certificate Bundle；
- HTTPS OTA 正常升级；
- 错误密码；
- 无效证书；
- CANCEL OTA；
- OTA 中长按关机；
- OTA 中临界低电保护。

Phase 11 将以这份列表作为整机功能对齐的主要输入。
