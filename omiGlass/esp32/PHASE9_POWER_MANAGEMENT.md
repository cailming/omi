# Phase 9：Power Management

## 目标

把原 Arduino 固件中分散的电源逻辑迁移为 ESP-IDF v6.1 原生 Power Management，并统一处理：

- CPU 动态频率
- Power Save
- 自动 Light Sleep
- Deep Sleep
- 长按关机
- GPIO1 唤醒
- Low Battery / Critical Battery
- Audio / Photo 对睡眠的约束

ESP-IDF 基线固定为 `v6.1.x`。

---

## 1. 为什么不能直接照搬 Arduino 的 Light Sleep

原 Arduino 固件直接调用：

```cpp
esp_sleep_enable_timer_wakeup(...);
esp_light_sleep_start();
```

同时希望维持 BLE 连接。

ESP-IDF v6.1 官方文档明确说明：

```text
手工进入 Light Sleep 时 Bluetooth 无法维持连接。
```

如果希望 BLE 连接仍然存在，应使用：

```text
Bluetooth Modem Sleep
+
ESP-IDF Power Management
+
FreeRTOS Tickless Idle
+
Automatic Light Sleep
```

因此 Phase 9 不再手工调用 `esp_light_sleep_start()`，而是迁移为官方推荐的自动 Light Sleep 模式。

---

## 2. sdkconfig.defaults

新增：

```text
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_80=y
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3
CONFIG_FREERTOS_HZ=1000
CONFIG_PM_SLP_IRAM_OPT=y
CONFIG_PM_RTOS_IDLE_OPT=y
```

BLE 省电相关：

```text
CONFIG_ESP_PHY_MAC_BB_PD=y
CONFIG_BT_CTRL_MODEM_SLEEP=y
CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y
CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y
CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=y
```

目标是让 ESP32-S3 在 BLE 广播/连接期间由控制器自己决定无线电唤醒时机，而不是业务代码粗暴暂停整个 SoC。

---

## 3. CPU Dynamic Frequency Scaling

新增：

```text
main/power_service.h
main/power_service.cpp
```

使用：

```cpp
esp_pm_configure()
```

配置：

```text
Max CPU = 80MHz
Min CPU = 40MHz
Automatic Light Sleep = ON
```

原 Arduino 配置虽然还定义过 `MAX_CPU_FREQ_MHZ=100`，但实际普通运行调用的是：

```text
NORMAL_CPU_FREQ_MHZ = 80
MIN_CPU_FREQ_MHZ    = 40
```

而 ESP32-S3 的 `ESP_PM_CPU_FREQ_MAX` 合法档位为 80/160/240MHz，因此 ESP-IDF 迁移版固定使用 80/40MHz。

---

## 4. PM Lock

创建两个 Lock：

```text
ESP_PM_CPU_FREQ_MAX
ESP_PM_NO_LIGHT_SLEEP
```

### CPU Max Lock

以下情况保持 80MHz：

```text
Audio streaming
Photo uploading
BLE connected（保持原固件行为）
```

以下情况允许降低到 40MHz：

```text
BLE 断开且空闲 >= 45 秒
Low Battery 且当前没有 Audio / Photo 实时业务
```

### No-Light-Sleep Lock

以下情况禁止自动 Light Sleep：

```text
Audio streaming
Photo uploading
最近 5 秒有用户/连接活动
```

否则释放 Lock，让 FreeRTOS Tickless Idle 和 ESP-IDF 自动决定是否进入 Light Sleep。

---

## 5. 电源状态机

当前状态：

```text
ACTIVE
POWER_SAVE
LOW_BATTERY
POWERING_OFF
```

主要转换：

```text
正常运行
   ↓
ACTIVE

BLE断开 + 空闲45秒
   ↓
POWER_SAVE

Battery <= 3.4V
   ↓
LOW_BATTERY

长按2秒 / Battery <= 3.3V / 离线空闲5分钟
   ↓
POWERING_OFF
   ↓
Deep Sleep
```

---

## 6. Activity 判定

以下事件会刷新最近活动时间：

```text
短按按键
BLE Connect / Disconnect 状态变化
Audio streaming 从空闲变为活动
Photo upload 从空闲变为活动
```

Audio / Photo 持续运行期间会直接持有 No-Light-Sleep Lock，因此不依赖不断刷新 activity 时间。

---

## 7. 长按关机

原固件：

```text
按键持续 2 秒
 ↓
POWER_OFF_SEQUENCE
 ↓
约 800ms 快闪
 ↓
Deep Sleep
```

ESP-IDF 版保留该体验：

```text
2 秒长按
 ↓
POWERING_OFF
 ↓
200ms 半周期 LED 快闪
 ↓
800ms 后准备 Deep Sleep
```

但增加一个重要修正：

```text
必须先等用户松开 GPIO1
```

原因：Deep Sleep 使用 GPIO1 低电平作为唤醒条件。如果用户还按住按钮就睡眠，会立即被低电平重新唤醒，表现为“关机后马上重启”。

---

## 8. Deep Sleep 唤醒

ESP-IDF v6.1 使用新的 ext1 API：

```cpp
esp_sleep_enable_ext1_wakeup_io(
    1ULL << GPIO_NUM_1,
    ESP_EXT1_WAKEUP_ANY_LOW);
```

不再使用旧代码：

```cpp
esp_sleep_enable_ext0_wakeup(...)
```

GPIO1 在 ESP32-S3 上属于 RTC GPIO。

睡眠前配置：

```text
RTC GPIO Input
Internal Pull-up ON
Pull-down OFF
RTC_PERIPH 保持供电
```

按键按下后 GPIO1 变低，设备重新启动。

---

## 9. Deep Sleep 前资源释放

进入 Deep Sleep 前：

```text
Audio TX Queue 清空
 ↓
取消 Photo Upload / 归还 framebuffer
 ↓
停止 PDM Mic / I2S
 ↓
释放 Opus Encoder
 ↓
Camera deinit
 ↓
Battery ADC deinit
 ↓
NimBLE Host stop / deinit
 ↓
LED OFF
 ↓
Deep Sleep
```

这样不会依赖 Deep Sleep 强行切电来掩盖未释放资源的问题。

---

## 10. Low Battery

阈值保持原配置：

```text
LOW       <= 3.4V
CRITICAL  <= 3.3V
```

### LOW

进入：

```text
LOW_BATTERY
```

行为：

- 非实时业务允许 CPU 降至 40MHz
- LED 采用 1 秒半周期慢闪
- Audio / Photo 正在运行时仍允许保持 80MHz，避免直接破坏业务

### CRITICAL

进入：

```text
POWERING_OFF
```

随后执行 Deep Sleep，避免继续深度放电。

---

## 11. 5 分钟空闲 Deep Sleep

原 `config.h` 已定义：

```text
DEEP_SLEEP_THRESHOLD_MS = 300000
```

旧 Arduino 主循环并没有真正执行这条配置。

Phase 9 把它补齐：

```text
BLE disconnected
+
Audio idle
+
Photo idle
+
连续空闲 >= 5 分钟

=> Deep Sleep
```

这是基于原项目已存在配置补齐缺失行为，而不是新定义一个时间阈值。

---

## 12. 主循环调度

高带宽：

```text
Audio subscribed
或
Photo uploading

=> 1ms 主循环 delay
```

空闲：

```text
=> 20ms delay
```

目的是给 FreeRTOS Tickless Idle 更长的空闲窗口，由 ESP-IDF PM 框架决定是否进入自动 Light Sleep。

---

## 13. 真机验证重点

### CPU / DFS

日志应出现：

```text
Phase 9 Power Management 初始化完成：CPU=40~80MHz，自动 Light Sleep=启用
```

建议真机确认：

- BLE 连接/音频时性能正常
- 断开 45 秒后进入 POWER_SAVE

### 长按关机

验证：

```text
按住 >= 2秒
 → 关机快闪
 → 松手
 → Deep Sleep
```

随后再次按键：

```text
GPIO1 LOW
 → 唤醒
 → 正常重新启动
```

### Battery

- `<=3.4V`：LOW_BATTERY
- `<=3.3V`：自动 Deep Sleep

### Idle timeout

BLE 断开、无业务 5 分钟：

```text
IDLE_TIMEOUT
 → Deep Sleep
```

### Light Sleep

需要实际测电流确认自动 Light Sleep residency。

注意：

> “代码启用了自动 Light Sleep”不等于“当前业务负载下必然有很高的 Light Sleep 占比”。

Mic/I2S、BLE Event、主循环频率都会影响系统实际进入 Light Sleep 的时间比例。

---

## 当前验证边界

本阶段目前只声明：

> **Phase 9 Power Management 代码迁移完成。**

尚未声明：

- ESP-IDF v6.1 `idf.py build` 已通过
- PM Kconfig 已由真实编译验证
- 40/80MHz DFS 已真机测量
- BLE + Automatic Light Sleep 已测电流
- GPIO1 Deep Sleep 唤醒已真机验证
- 临界低电保护已在真实电池上验证
