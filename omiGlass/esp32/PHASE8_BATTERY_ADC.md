# Phase 8：Battery ADC

## 目标

把原 Arduino 固件中的 Battery ADC 检测迁移到 ESP-IDF v6.1 原生 ADC 驱动，并把真实电量接回标准 BLE Battery Service：

```text
Battery Service       0x180F
Battery Level         0x2A19
Properties            READ + NOTIFY
```

本阶段只负责：

- ADC 检测
- 电压换算
- 百分比计算与平滑
- Battery Level Read / Notify

低电降频、临界电压关机、Light Sleep 等电源策略统一留到 Phase 9。

---

## 1. 原 Arduino 实现

原固件使用：

```cpp
analogReadResolution(12);
analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
```

Battery GPIO：

```text
GPIO2 / A1
```

检测时连续读取 10 次：

```text
10 samples
每次间隔 10ms
```

原始电压计算：

```text
ADC raw
   ↓
raw / 4095 * 3.3V
   ↓
× 6.086 分压比
   ↓
Battery Voltage
```

---

## 2. ESP-IDF v6.1 实现

新增：

```text
main/battery_service.h
main/battery_service.cpp
```

CMake 新增依赖：

```text
esp_adc
```

使用头文件：

```cpp
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
```

数据链路：

```text
GPIO2
  ↓
adc_oneshot_io_to_channel()
  ↓
ADC Unit / Channel
  ↓
adc_oneshot_read() × 10
  ↓
raw average
  ↓
adc_cali_raw_to_voltage()
  ↓
校准后的 ADC mV
  ↓
× 6.086
  ↓
Battery Voltage
  ↓
Battery Percentage
  ↓
BLE 0x2A19
```

---

## 3. ADC Channel 不硬编码

ESP-IDF 版本没有直接写死：

```text
ADC1_CHANNEL_x
```

而是使用：

```cpp
adc_oneshot_io_to_channel(GPIO2, &unit, &channel);
```

这样 GPIO 与 ADC channel 的映射由 ESP-IDF / SoC 定义负责。

---

## 4. ADC 配置

保持原 Arduino 的 12-bit + 最大衰减档意图：

```text
Bit width     ADC_BITWIDTH_12
Attenuation   ADC_ATTEN_DB_12
```

Arduino Core 中原先使用的：

```text
ADC_11db
```

在当前 ESP-IDF v6.1 / ESP32-S3 原生 API 中对应使用新的最大衰减枚举 `ADC_ATTEN_DB_12`。

---

## 5. ADC 校准

ESP32-S3 使用 Curve Fitting calibration：

```cpp
adc_cali_create_scheme_curve_fitting()
adc_cali_raw_to_voltage()
```

相比 Arduino 原来的：

```text
raw / 4095 * 3.3V
```

优先使用芯片校准数据能降低芯片间 ADC 参考电压差异造成的误差。

如果校准初始化失败，代码不会让 Battery Service 整体失效，而是回退到：

```text
raw / 4095 * 3300mV
```

回退路径主要用于诊断和兼容；正常 ESP32-S3 应优先使用 Curve Fitting。

---

## 6. 电池硬件参数

严格保留原固件参数：

```text
Voltage Divider Ratio   6.086
Battery Max             4.2V
Battery Min             3.2V
Critical Voltage        3.3V
Low Voltage             3.4V
```

最终电池电压仍做合理范围保护：

```text
2.5V ~ 5.0V
```

---

## 7. Battery Percentage

保持原 Arduino 百分比计算：

```text
>= 4.2V  -> 100%
<= 3.2V  -> 0%
其余      -> 3.2V~4.2V 线性换算
```

并保持原来的平滑逻辑：

```text
如果新结果与上一次相差 <= 5%
    使用新结果

如果相差 > 5%
    每次只向目标方向移动 2%
```

例如：

```text
上次 80%
新计算 65%

本次对外结果不是直接 65%
而是 78%
```

这样避免 ADC 抖动导致手机端电量快速跳变。

---

## 8. 检测周期

保持原配置：

```text
内部 Battery Check    20 秒
Battery Report        90 秒（保留配置，后续电源管理可使用）
```

启动阶段立即执行第一次检测。

之后：

```text
每 20 秒
   ↓
Battery ADC
   ↓
更新最新百分比
   ↓
如果 Battery Notify 已订阅
   ↓
发送 1-byte Battery Level
```

---

## 9. BLE Battery Service

Phase 4 创建的占位值已经移除。

`0x2A19` 现在读取：

```text
s_battery_level
```

范围：

```text
0 ~ 100
```

BLE 层使用原子变量保存 Battery Level，因为：

```text
app_main task
    ↓ 更新百分比

NimBLE Host Task
    ↓ GATT Read
```

两个执行上下文可能同时访问该值。

### Notify

新增：

```cpp
ble_service_update_battery_level(percentage, notify_if_subscribed)
```

行为：

```text
始终更新 GATT Read 最新值

如果：
connected
+ Battery Notify subscribed
+ notify_if_subscribed=true

则同时发送 1-byte Notify
```

手机刚开启 Battery Notify 时，业务主循环会立即推送最近一次有效结果，而不用等待下一个 20 秒周期。

---

## 10. 当前职责边界

Phase 8 已经定义并保留：

```text
LOW       <= 3.4V
CRITICAL  <= 3.3V
```

但目前只打印警告。

不会在 Phase 8 中：

- 自动降低 CPU 频率
- 停止 Camera
- 关闭 Audio
- 自动进入 Deep Sleep
- 强制关机

这些统一由 Phase 9 Power Manager 处理。

---

## 11. 真机验证清单

### ADC

1. `battery_service_init()` 成功。
2. GPIO2 能通过 `adc_oneshot_io_to_channel()` 找到 ADC unit/channel。
3. Curve Fitting calibration 创建成功。
4. 每次检测可以完成 10 次 ADC read。
5. raw average 在合理范围内。
6. ADC mV 不异常跳变。

### 电压

使用万用表测实际电池电压，与日志比较：

```text
Battery：raw=...
adc=...mV
voltage=...V
percentage=...%
calibrated=是
```

重点验证：

```text
Battery Voltage ≈ 万用表电压
```

原项目的 6.086 是人工校准后的分压比，因此迁移到 ADC calibration 后仍建议重新实测一次误差。

### BLE

使用 nRF Connect 或现有 Omi App：

1. 连接 `OMI Glass`。
2. 找到 Battery Service `0x180F`。
3. 读取 Battery Level `0x2A19`。
4. 读取结果为 0~100 的 1 byte。
5. 打开 Notify。
6. 打开 Notify 后立即收到最近一次有效电量。
7. 后续约每 20 秒收到更新。

### 平滑

人工改变供电电压或模拟不同电压时，验证：

- 小变化可以正常更新。
- 大于 5% 的突变每次只移动约 2%。

---

## 12. Phase 9 接口

Phase 9 可以直接读取：

```cpp
BatteryReading reading = battery_service_latest();
```

并根据：

```text
battery_voltage
percentage
```

决定：

```text
正常模式
低电模式
临界电压保护
```

这样 Phase 9 不再直接访问 ADC 硬件。
