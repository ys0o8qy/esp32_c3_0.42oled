# 房间温湿度计 · ESP32-C3 72×40 OLED

每个房间一块开发板。每秒读取 SHT3x 并显示温湿度，通过手机配置 Wi-Fi、房间名及接口服务，默认每 60 秒上传一组最新读数。接口服务由用户提供，本项目不搭建服务端。

## 接线与依赖

- 板载 0.42 寸 SSD1306 72×40 OLED；使用 U8g2 专用驱动处理显存偏移。
- SHT3x：SDA → GPIO5，SCL → GPIO6，VCC → 3.3V，GND → GND。与 OLED 共用 I2C，代码引脚须与实物一致。
- Wi-Fi 工作会产生热量，传感器应与开发板和 OLED 保持一定距离，避免把板上温升当成室温。
- BOOT 配置按钮：GPIO9，低电平按下；运行时长按 3 秒进入配置。不要在开机或复位时按住，否则会进入芯片下载模式。
- ESP32 Arduino Core 3.3.10、U8g2 2.36.19、Sensirion I2C SHT3x 1.0.1、Sensirion Core 0.7.3。联网、网页、NVS、HTTP 和证书库均来自 ESP32 Core。

## 首次使用：手机配置

1. 上传固件后上电。没有 Wi-Fi 配置时，自动创建 `C3-XXXXXX` 热点。
2. OLED 显示热点名和 `P:` 后的随机 8 位密码；每次重新开启配置热点会更换密码。配置说明显示 6 秒、温湿度显示 2 秒，循环切换。
3. 手机连接该热点。如果提示“无互联网”，选择保持连接；浏览器打开 **http://192.168.4.1/**。部分手机会自动弹出配置页。
4. 填写房间名称、家中 **2.4GHz Wi-Fi** 名称和密码。接口地址和 Bearer Token 可以先留空；间隔默认 60 秒，可设 10–86400 秒。
5. 点击“保存并应用”，观察页面上方的 Wi-Fi 和上报状态。确认后点击“结束配置，关闭热点”，手机即可连接回原 Wi-Fi。

配置断电保存，无需每次重新填写。修改配置时，在设备运行期间长按 BOOT 3 秒，重新连接屏幕显示的热点。保存过 Wi-Fi 配置后，配置热点会在 10 分钟后关闭；普通断网不会自动开放配置热点，设备会持续重连。

同一 Wi-Fi 下密码留空表示保留；换 Wi-Fi 请填写新密码。无密码网络须勾选对应选项。Token 留空表示保留，需要删除时勾选“清除已保存的 Token”。页面不回显已保存的密码和 Token；修改配置只开放在设备热点接口上。

## 接口约定

- 方法：`POST`，`Content-Type: application/json`。
- 配置 Token 后附加 `Authorization: Bearer <token>`。
- 支持 HTTP 与 HTTPS；HTTPS 使用 ESP32 内置公共根证书集合进行验证，不跳过校验。自签名证书/私有 CA 尚未支持。HTTP 会明文传输数据和 Token。
- 地址最长 256 字节，支持域名、IPv4、端口和路径/查询参数，不支持 URL 内嵌账号密码、片段及 IPv6 字面量。
- 房间名最长 63 个 UTF-8 字节（通常最多 21 个汉字）；设备 ID 来自出厂 Wi-Fi MAC，重新配网、更改房间及重启不会改变。
- 任意 `2xx` 视为接收成功。`3xx` 不自动跟随重定向，避免认证信息发往其他地址；请填写最终接收地址。

```json
{
  "device_id": "c3-a1b2c3d4e5f6",
  "room": "卧室",
  "temperature": 26.30,
  "humidity": 48.20,
  "sampled_at": "2026-10-10T08:30:00Z"
}
```

`temperature` 为 °C，`humidity` 为 %RH。`sampled_at` 是采样时刻的 UTC 时间，不是上报完成时间。联网后通过 NTP 校时；未校时时 HTTP 上报省略该字段，服务端应记录接收时间；HTTPS 为正确验证证书会等待校时。默认 NTP 服务器在 `NetworkService.cpp` 的 `configTime()` 中配置。

接口地址留空时不发送请求。首次连网/恢复连接/应用新配置后立即尝试上报有效读数；成功后按配置周期上报。失败后按 15、30、60、120、240 秒退避重试，成功后恢复正常周期。只保留最新样本：不缓存历史、不补传过期数据，传感器失败或样本超过 3 秒不发起新上报。

正在进行的旧请求可能在保存新配置后才结束；随后应用新配置。串口会输出 HTTP 状态码（`-1` 表示连接/TLS/超时等传输失败），不输出请求 Token。由于网络可能在服务器接收后中断，应由服务端按需要去重。

## OLED 状态

正常时两行显示 `T` 温度、`H` 湿度，保留一位小数。底部交替显示：

- `LIVE 000001`：成功采样计数，小点每次成功采样交替亮灭。
- `WiFi offline`：尚未联网，仍继续本地测量。
- `API not set`：已联网，但没有配置上报地址。
- `Sending...` / `UP n WiFi+`：正在上传 / 本次开机成功上传次数。
- `Retry 401` 等：失败后等待重试，数字为 HTTP 状态码。
- `Sync clock`：HTTPS 上报前等待网络校时。

传感器异常显示 `SHT missing`、`SHT init` 或 `I2C read`，每秒重试。OLED 初始化失败不影响采样和网络，修复屏幕后重启。倒置时将构造函数的 `U8G2_R0` 改为 `U8G2_R2`。

## 构建与上传

在本目录运行：

```sh
arduino-cli lib install "U8g2@2.36.19" "Sensirion I2C SHT3x@1.0.1" "Sensirion Core@0.7.3"
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=dio,UploadSpeed=115200 --build-path /tmp/c3-connected-build .
arduino-cli board list
arduino-cli upload --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=dio,UploadSpeed=115200 --port '<PORT>' --input-dir /tmp/c3-connected-build .
arduino-cli monitor --port '<PORT>' --config baudrate=115200
```

Arduino IDE：ESP32C3 Dev Module、4MB Flash、默认分区、USB CDC On Boot Enabled、Flash Mode DIO。USB-UART 桥接版则改为 CDC Disabled。将 `<PORT>` 替换为实际串口；上传前关闭占用端口的串口监视器。后续普通上传保留 NVS 配置，Erase All Flash 会清除配置。

## 实现与验证

`c3_temperature.ino` 负责按键、SHT3x 及 OLED；`NetworkService.cpp` 分别用独立 FreeRTOS 任务处理手机网页和网络连接/上报，网络等待不会阻塞主循环采样；队列只保存最新样本。`PortalPage.h` 是离线可用、无外部资源的手机页面，`Telemetry.h` 是可在电脑上验证的格式及调度逻辑。配置以版本化 NVS blob 保存，错误输入不会覆盖现有配置。

```sh
c++ -std=c++17 tests/telemetry_test.cpp -o /tmp/c3-telemetry-test
/tmp/c3-telemetry-test
```

自动检查覆盖 JSON 转义、非法 URL/Token、间隔范围、失效样本、毫秒计数回绕和退避。手机页面用本机模拟接口测试过 390px 布局、保存反馈、密码/Token 清空、错误反馈和关闭反馈；模拟接口不代表开发板网络端到端验证。

使用 Git 管理源码和变更，不生成固件备份；真实配置只通过手机输入，不提交到仓库。真实 Wi-Fi、NVS 重启恢复、HTTPS 证书校验和生产接口接收仍需配置后验证。

参考：[Sensirion 官方库](https://github.com/Sensirion/arduino-i2c-sht3x)、[ESP32 Wi-Fi API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html)、[Preferences API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/preferences.html)。

### 本次验证（2026-10-10）

- 最终源码通过 ESP32-C3 编译：程序 1288625 字节，静态内存 40516 字节；默认应用分区使用约 98%，后续扩展较多时需评估分区空间。
- 已烧录到当前 USB 连接的小屏开发板，Flash 哈希校验通过。
- 启动日志确认配置 AP 创建成功；I2C 检测到 OLED 0x3C 和 SHT3x 0x44；联网任务运行期间连续温湿度采样正常（约 27.4°C、54%RH）。
- 主机自动检查和 390px 手机页面模拟交互通过。真实手机连接、Wi-Fi 配置保存/重启恢复及生产接口上报需用户配置后继续验收。
