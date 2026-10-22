# Phase 10：Wi-Fi / HTTPS OTA

## 目标

把原 Arduino 固件中的 OTA 实现迁移为 ESP-IDF v6.1 原生网络与 OTA 能力，同时保持手机端 BLE OTA 协议不变。

原 Arduino 依赖：

```text
WiFi.h
WiFiClientSecure.h
HTTPClient.h
Update.h
```

ESP-IDF v6.1 迁移后：

```text
esp_wifi
esp_netif
esp_event
esp_http_client
esp_https_ota
esp_ota_ops / app_update
ESP x509 Certificate Bundle
```

本阶段最重要的安全变化：

> 不再使用 `setInsecure()`；OTA 只接受 HTTPS，并校验服务器证书与主机名。

---

## 1. BLE OTA 协议保持不变

### Service / Characteristic UUID

```text
OTA Service
19B10010-E8F2-537E-4F6C-D104768A1214

OTA Control
19B10011-E8F2-537E-4F6C-D104768A1214
READ + WRITE

OTA Data
19B10012-E8F2-537E-4F6C-D104768A1214
READ + NOTIFY
```

### 命令字

```text
0x01 SET_WIFI
0x02 START_OTA
0x03 CANCEL_OTA
0x04 GET_STATUS
0x05 SET_URL
```

### 状态码

```text
0x00 IDLE

0x10 WIFI_CONNECTING
0x11 WIFI_CONNECTED
0x12 WIFI_FAILED

0x20 DOWNLOADING
0x21 DOWNLOAD_COMPLETE
0x22 DOWNLOAD_FAILED

0x30 INSTALLING
0x31 INSTALL_COMPLETE
0x32 INSTALL_FAILED

0x40 REBOOTING
0xFF ERROR
```

OTA Control / OTA Data 读取和 OTA Data Notify 均使用：

```text
byte 0  status
byte 1  progress 0~100
```

---

## 2. SET_WIFI 协议

保持原格式：

```text
[cmd]
[ssid_len]
[ssid...]
[password_len]
[password...]
```

即：

```text
0x01 | SSID长度 | SSID | 密码长度 | 密码
```

限制：

```text
SSID      <= 32 bytes
Password  <= 64 bytes
```

ESP-IDF 版使用：

```cpp
esp_wifi_set_storage(WIFI_STORAGE_RAM);
```

因此本阶段 Wi-Fi 凭据：

- 只保存在 RAM；
- 不写 Wi-Fi NVS；
- 重启后消失；
- 密码不输出到日志。

这与“手机临时下发 OTA 网络凭据”的业务语义更一致。

---

## 3. SET_URL 协议

保持原格式：

```text
[cmd]
[url_len_hi]
[url_len_lo]
[url...]
```

URL 长度采用大端 16-bit：

```text
url_len = (byte1 << 8) | byte2
```

当前最大：

```text
256 bytes
```

### HTTPS 强制要求

ESP-IDF 版本只接受：

```text
https://...
```

以下地址会直接拒绝：

```text
http://...
ftp://...
其他非 HTTPS scheme
```

这属于有意的安全增强，不改变 BLE 线协议。

### URL 日志保护

OTA URL 可能是：

```text
https://server/file.bin?token=...&signature=...
```

因此代码不会打印完整 URL，只记录：

```text
OTA HTTPS URL 已更新，长度=xxx
```

避免把预签名 URL、临时 token 或 query signature 写进设备日志。

---

## 4. HTTPS 证书验证

原 Arduino 代码：

```cpp
WiFiClientSecure client;
client.setInsecure();
```

该方式不验证服务器身份，存在中间人攻击风险。

Phase 10 改为：

```cpp
http_config.crt_bundle_attach = esp_crt_bundle_attach;
```

并启用：

```text
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=n
```

ESP-IDF 会使用内置 Mozilla Root CA Bundle 验证服务器证书链。

代码没有设置：

```text
skip_cert_common_name_check
```

因此主机名验证同样保持开启。

---

## 5. OTA Task 模型

BLE OTA Control 的 GATT Write 回调运行在 NimBLE Host Task。

不能在该回调中执行：

```text
Wi-Fi connect
TLS handshake
HTTP download
Flash erase / write
```

否则会长时间阻塞 BLE Host。

因此架构为：

```text
手机
 ↓
OTA Control Write
 ↓
NimBLE Host Task
 ↓
快速解析命令
 ↓
ota_service_handle_command()
 ↓
xTaskCreate(omi_https_ota)
 ↓
独立 OTA FreeRTOS Task
```

OTA Task stack：

```text
8192 bytes
```

优先级：

```text
5
```

---

## 6. Wi-Fi STA

`ota_service_init()` 只建立：

```text
esp_netif
Default Event Loop
STA netif
Wi-Fi / IP Event Handler
```

不会主动打开 Wi-Fi Radio。

真正收到 START_OTA 后才：

```text
esp_wifi_init()
 ↓
esp_wifi_set_storage(WIFI_STORAGE_RAM)
 ↓
esp_wifi_set_mode(WIFI_MODE_STA)
 ↓
esp_wifi_set_config()
 ↓
esp_wifi_start()
 ↓
esp_wifi_connect()
```

连接成功由：

```text
IP_EVENT_STA_GOT_IP
```

确认。

连接超时：

```text
15 秒
```

状态：

```text
开始连接 -> 0x10 WIFI_CONNECTING
获取 IP   -> 0x11 WIFI_CONNECTED
失败/超时 -> 0x12 WIFI_FAILED
```

---

## 7. Wi-Fi / BLE 共存

OTA 期间需要同时保持：

```text
BLE GATT Control / Status
+
Wi-Fi STA HTTPS Download
```

启用：

```text
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
```

ESP32-S3 的 Wi-Fi 和 BLE 共用 2.4GHz Radio，ESP-IDF coexistence 模块负责调度无线资源。

### OTA 维护模式

为了进一步降低竞争和内存压力，OTA 执行期间暂停高带宽业务数据面：

```text
Mic PDM       暂停
Opus Encode   暂停
BLE Audio TX  清空并暂停
Photo Capture 暂停
Photo Upload  取消并释放 framebuffer
```

仍然保留：

```text
BLE OTA Control
BLE OTA Status Notify
Battery ADC
Power Management 协调
Wi-Fi HTTPS OTA
```

OTA 失败或用户取消后，如果没有待关机条件，则恢复麦克风实时业务。

---

## 8. ESP HTTPS OTA 链路

核心链路：

```text
HTTPS URL
 ↓
esp_https_ota_begin()
 ↓
读取新固件 App Description
 ↓
esp_https_ota_perform()
 ↓
边下载边写 inactive OTA partition
 ↓
检查是否完整接收
 ↓
esp_https_ota_finish()
 ↓
设置新 OTA boot partition
 ↓
BLE INSTALL_COMPLETE
 ↓
BLE REBOOTING
 ↓
esp_restart()
```

### 固件信息

开始下载后读取：

```cpp
esp_https_ota_get_img_desc()
```

日志可显示：

```text
Firmware Version
Project Name
```

用于真机升级时确认实际下载的镜像。

---

## 9. OTA 进度

使用：

```cpp
esp_https_ota_get_image_size()
esp_https_ota_get_image_len_read()
```

如果 HTTP Server 提供固定 Content-Length：

```text
progress = bytes_read * 100 / image_size
```

与原固件一致，每：

```text
5%
```

向手机更新一次。

原 Arduino 实际执行过程中，HTTP GET 成功后直接进入 INSTALLING，再边网络读取边写 Flash；虽然定义了 `DOWNLOAD_COMPLETE=0x21`，原代码并没有实际发送该状态。

因此当前 native 版本继续保持手机实际可观察到的状态序列：

```text
DOWNLOADING 0%
 ↓
INSTALLING 0%
 ↓
INSTALLING 5%
 ↓
INSTALLING 10%
 ↓
...
 ↓
INSTALL_COMPLETE 100%
 ↓
REBOOTING 100%
```

如果服务器使用 chunked transfer、无法取得总镜像大小，则无法计算稳定百分比，中间不伪造进度，完成时再上报 100%。

---

## 10. OTA 完整性

下载结束后首先检查：

```cpp
esp_https_ota_is_complete_data_received()
```

如果镜像不完整：

```text
abort
 ↓
DOWNLOAD_FAILED
```

完整后才调用：

```cpp
esp_https_ota_finish()
```

`finish()` 负责完成镜像校验和 OTA boot partition 切换。

如果失败：

```text
INSTALL_FAILED
```

不会重启进入不完整固件。

---

## 11. CANCEL_OTA

手机发送：

```text
0x03
```

后：

```text
s_cancel_requested = true
```

OTA Task 在安全检查点执行：

```cpp
esp_https_ota_abort()
```

随后：

```text
停止 Wi-Fi
 ↓
busy=false
 ↓
状态回 IDLE
 ↓
恢复实时业务
```

本阶段没有直接杀死正在写 Flash 的 OTA Task，避免在底层 Flash 操作处于未知状态时强制删除 Task。

---

## 12. Phase 9 Power Management 联动

### OTA 期间禁止普通睡眠

OTA busy 时 Power Service 将其视作高带宽活动：

```text
CPU Max Lock
+
No-Light-Sleep Lock
```

因此 TLS / HTTP / Flash Write 期间不会进入自动 Light Sleep。

### 长按关机

如果 OTA 正在写 Flash 时用户长按：

```text
Long Press
 ↓
先 ota_service_cancel()
 ↓
等待 OTA Task 安全退出
 ↓
再交给 Power Service
 ↓
Deep Sleep
```

不会在写 Flash 中途直接切进 Deep Sleep。

### Critical Battery

如果 OTA 期间电池 <= 3.3V：

```text
先取消 OTA
 ↓
等待 OTA Task 退出
 ↓
再执行 Critical Battery Deep Sleep
```

同样避免 OTA 分区正在写入时突然休眠。

如果 OTA 因关机/临界低电退出，不会先恢复 PDM Mic 再马上关机。

---

## 13. BLE 层职责

BLE 模块不知道：

```text
Wi-Fi SSID/password 如何保存
Wi-Fi 如何连接
TLS 如何验证
OTA 分区如何写
```

它只负责：

```text
OTA Control Write
 ↓
BleOtaCommandHandler
 ↓
ota_service
```

反向：

```text
ota_service status
 ↓
app_main bridge
 ↓
ble_service_update_ota_status()
 ↓
OTA Data Notify
```

因此 BLE 与 OTA 网络实现保持解耦。

---

## 14. 分区表

当前仍保持 Arduino 基线：

```text
app0  0x1C0000
app1  0x1C0000
```

虽然硬件配置为：

```text
8MB Flash
```

但现有 inherited partition table 实际只使用前约 4MB。

Native ESP-IDF 加入：

```text
NimBLE
Wi-Fi
TLS
Root CA Bundle
Camera
Opus
```

后固件体积可能明显增加。

因此 Phase 10 暂不盲目修改分区表，先由 ESP-IDF v6.1 CI 的实际 binary size 决定：

```text
如果 <= 0x1C0000
    保持现状

如果超过 / 剩余空间过小
    再利用完整 8MB Flash 扩大双 OTA Slot
```

任何分区表调整都必须继续保留：

```text
otadata
app0
app1
```

才能维持 A/B OTA。

---

## 15. 安全边界

Phase 10 明确做到：

- HTTPS only；
- X.509 server verification；
- hostname verification；
- 不使用 `setInsecure()`；
- Wi-Fi password 不写日志；
- Wi-Fi credentials 不写 NVS；
- 完整 OTA URL 不写日志；
- 下载不完整不切换启动分区；
- OTA 写 Flash 时不直接 Deep Sleep。

当前尚未加入：

- Secure Boot；
- Flash Encryption；
- Signed App Verification / anti-rollback；
- 新固件启动后的自动健康检查 + rollback confirmation。

这些属于产品化安全加固，不应在协议迁移阶段偷偷改变现有部署模型；可在 Phase 11/后续安全加固阶段单独设计。

---

## 16. 真机验证矩阵

### A. GATT 协议

验证：

```text
OTA Service UUID 不变
OTA Control READ+WRITE
OTA Data READ+NOTIFY
GET_STATUS 返回 2 bytes
```

### B. Wi-Fi 成功

```text
SET_WIFI
SET_URL
START
```

预期：

```text
WIFI_CONNECTING
WIFI_CONNECTED
DOWNLOADING
INSTALLING ...
```

### C. Wi-Fi 密码错误

预期：

```text
WIFI_CONNECTING
WIFI_FAILED
```

并且：

```text
设备不重启
旧固件正常保留
```

### D. HTTP URL

传入：

```text
http://server/firmware.bin
```

预期：

```text
ERROR
```

不会建立明文 OTA 连接。

### E. 无效 TLS Certificate

使用：

```text
self-signed / hostname mismatch / expired cert
```

且根证书不在信任链。

预期：

```text
TLS verification fail
DOWNLOAD_FAILED
```

不能像旧 `setInsecure()` 一样继续下载。

### F. CANCEL

下载/安装进度中发送：

```text
CANCEL_OTA
```

预期：

```text
abort
Wi-Fi stop
IDLE
恢复实时业务
旧固件继续运行
```

### G. 截断镜像

服务器提前断开。

预期：

```text
complete_data_received = false
DOWNLOAD_FAILED / INSTALL_FAILED
不切换 boot partition
```

### H. 成功升级

预期：

```text
INSTALL_COMPLETE 100
REBOOTING 100
esp_restart()
```

重启后：

```text
Firmware Revision
```

应变为新版本。

### I. OTA + 长按

OTA 写入过程中长按 2 秒。

预期：

```text
先取消 OTA
等待 Task 结束
再 Deep Sleep
```

### J. OTA + Critical Battery

OTA 过程中模拟：

```text
Battery <= 3.3V
```

预期同样是：

```text
cancel first
then Deep Sleep
```

---

## 17. CI 编译验证

GitHub Actions 已固定：

```text
espressif/idf:v6.1
```

并执行：

```bash
source "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py build
```

早期 CI 失败曾是 GitHub Actions 覆盖官方 Docker entrypoint，导致 `idf.py` 没加入 PATH；工作流已经显式加载 `export.sh` 修复。

当前工作流还会在构建失败时把 `CMake Error / error: / fatal error:` 等关键行转换成 GitHub Annotation，便于在 Chat 模式下继续形成“提交 -> 编译 -> 修错”的闭环。

最终是否可声明 `idf.py build` 通过，以对应分支最新 commit 的 CI 结果为准。

---

## 18. Phase 10 新增/修改文件

新增：

```text
main/ota_service.h
main/ota_service.cpp
PHASE10_HTTPS_OTA.md
```

修改：

```text
main/omiglass_config.h
main/ble_service.h
main/ble_service.cpp
main/app_main.cpp
main/CMakeLists.txt
sdkconfig.defaults
.github/workflows/omiglass-esp-idf.yml
```

原 Arduino：

```text
omiGlass/firmware/
```

继续保持不修改。
