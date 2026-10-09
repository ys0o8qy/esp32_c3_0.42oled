# ESP32-C3 小屏温湿度计

适用于已确认的板载 0.42 寸、SSD1306 72×40 OLED，SHT3x 与屏幕共用 I2C：SDA=GPIO5、SCL=GPIO6。传感器 VCC 接 3.3V，GND 接 GND；代码引脚必须与物理接线一致。

使用 U8g2 的 `U8G2_SSD1306_72X40_ER_F_HW_I2C` 驱动处理屏幕分辨率及显存偏移，不沿用 128×64 屏幕设置。参考 [01Space 板卡资料](https://github.com/01Space/ESP32-C3-0.42LCD)。

屏幕分两行显示温度（T，摄氏度）和湿度（H，相对湿度百分比），各保留一位小数，底部显示成功采样计数（如 `LIVE 000001`），右侧小点每次成功采样交替亮灭。即使温湿度数值相同，计数仍会增加；显示计数每一百万次循环，读取失败时改为错误画面。每秒采样一次，自动识别 SHT3x 地址 0x44/0x45 和 OLED 地址 0x3C/0x3D。使用 [Sensirion 官方 SHT3x 库](https://github.com/Sensirion/arduino-i2c-sht3x)处理测量命令、CRC 校验、命令等待和数值换算，保留错误提示及传感器自动重试；OLED 初始化失败后仍会输出串口读数，修复屏幕后需要重启。

## 编译和上传

依赖 ESP32 Arduino Core、U8g2、Sensirion I2C SHT3x 1.0.1 和 Sensirion Core 0.7.3；本机已安装。开发板选择 **ESP32C3 Dev Module**。原生 USB 连接一般启用 USB CDC On Boot；若使用外置 USB-UART 桥接芯片，则关闭该选项。

```sh
arduino-cli lib install "Sensirion I2C SHT3x@1.0.1" "Sensirion Core@0.7.3"
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=dio,UploadSpeed=115200 --build-path /tmp/c3-small-oled-build .
arduino-cli board list
```

确认串口属于这块小屏开发板后上传（将 `<PORT>` 替换为真实串口）：

```sh
arduino-cli upload --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=dio,UploadSpeed=115200 --port '<PORT>' --input-dir /tmp/c3-small-oled-build .
arduino-cli monitor --port '<PORT>' --config baudrate=115200
```

若屏幕倒置，将构造函数中的 `U8G2_R0` 改为 `U8G2_R2`。错误画面用短文本适配小屏：`SHT missing` 为未找到传感器，`SHT init` 为初始化失败，`I2C read` 为测量失败（包括 CRC 错误）。串口会输出官方库的具体错误说明和错误码；失败后每秒重新查找、初始化并重试。

## 版本与验证

通过本目录 Git 管理，不额外备份源码或固件。此前手写驱动曾在另一块 ESP32C3-Pro 实机验证；切换官方库后仍需重新确认本板实际显示、方向与传感器读数。

此前官方库版本已通过 ESP32-C3 编译，用户确认温湿度读数会更新。动态 LIVE 指示的编译和烧录状态见下方记录。

动态 LIVE 指示已通过 ESP32-C3 编译：程序 339606 字节，静态内存 15760 字节。尚未烧录或验证动态指示的实物显示。
