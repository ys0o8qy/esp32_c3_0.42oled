# ESP32-C3 小屏温湿度计

适用于已确认的板载 0.42 寸、SSD1306 72×40 OLED，SHT3x 与屏幕共用 I2C：SDA=GPIO5、SCL=GPIO6。传感器 VCC 接 3.3V，GND 接 GND；代码引脚必须与物理接线一致。

使用 U8g2 的 `U8G2_SSD1306_72X40_ER_F_HW_I2C` 驱动处理屏幕分辨率及显存偏移，不沿用 128×64 屏幕设置。参考 [01Space 板卡资料](https://github.com/01Space/ESP32-C3-0.42LCD)。

屏幕分两行显示温度（T，摄氏度）和湿度（H，相对湿度百分比），各保留一位小数，底部显示 `LIVE 1s`。每秒采样一次，自动识别 SHT3x 地址 0x44/0x45 和 OLED 地址 0x3C/0x3D。包含 CRC 校验、关闭加热器后的 2ms 等待、错误提示及传感器自动重试；OLED 初始化失败后仍会输出串口读数，修复屏幕后需要重启。

## 编译和上传

依赖 ESP32 Arduino Core 和 U8g2；本机已安装。开发板选择 **ESP32C3 Dev Module**。原生 USB 连接一般启用 USB CDC On Boot；若使用外置 USB-UART 桥接芯片，则关闭该选项。

```sh
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=dio,UploadSpeed=115200 --build-path /tmp/c3-small-oled-build .
arduino-cli board list
```

确认串口属于这块小屏开发板后上传（将 `<PORT>` 替换为真实串口）：

```sh
arduino-cli upload --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=dio,UploadSpeed=115200 --port '<PORT>' --input-dir /tmp/c3-small-oled-build .
arduino-cli monitor --port '<PORT>' --config baudrate=115200
```

若屏幕倒置，将构造函数中的 `U8G2_R0` 改为 `U8G2_R2`。错误画面用短文本适配小屏，完整原因见串口。

## 版本与验证

通过本目录 Git 管理，不额外备份源码或固件。原温湿度逻辑已在另一块 ESP32C3-Pro 实机验证；本小屏版本需单独确认实际显示、方向与传感器读数，不把另一块板的验证当成本板验证。

小屏版本已通过 ESP32-C3 编译（USB CDC 开启）：程序 335478 字节，静态内存 15736 字节。尚未烧录或完成本板实物显示验证。
