#include <Wire.h>
#include <U8g2lib.h>

constexpr int OLED_SDA = 5;
constexpr int OLED_SCL = 6;

// 0.42-inch OLED: 72x40 pixels; the driver handles the controller offset.
// Change U8G2_R0 to U8G2_R2 only if the physical display is upside down.
U8G2_SSD1306_72X40_ER_F_HW_I2C oled(
    U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
bool oledReady = false;
uint8_t sensorAddress = 0;
uint32_t lastSampleMs = 0;
constexpr uint32_t SAMPLE_INTERVAL_MS = 1000;

// Sensirion SHT3x-DIS CRC-8: polynomial 0x31, initial value 0xFF.
uint8_t crc8(const uint8_t *data, size_t length) {
  uint8_t crc = 0xFF;
  while (length--) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
    }
  }
  return crc;
}

bool sendSensorCommand(uint16_t command) {
  Wire.beginTransmission(sensorAddress);
  Wire.write(static_cast<uint8_t>(command >> 8));
  Wire.write(static_cast<uint8_t>(command));
  const uint8_t result = Wire.endTransmission();
  if (result) Serial.printf("SHT3x address=0x%02X command=0x%04X I2C error=%u\n",
                            sensorAddress, command, result);
  return result == 0;
}

bool respondsAt(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool connectSensor() {
  sensorAddress = respondsAt(0x44) ? 0x44 : (respondsAt(0x45) ? 0x45 : 0);
  if (!sensorAddress) return false;
  if (!sendSensorCommand(0x30A2)) return false; // Soft reset.
  delay(10);
  if (!sendSensorCommand(0x3066)) return false; // Heater off.
  delay(2); // Allow the heater command to finish before starting a measurement.
  return true;
}

const char *readSensor(float &temperature, float &humidity) {
  if (!sensorAddress && !connectSensor()) return "SHT3x missing";
  // High repeatability, single shot, without clock stretching.
  if (!sendSensorCommand(0x2400)) return "Command failed";
  delay(20);
  uint8_t data[6];
  if (Wire.requestFrom(sensorAddress, static_cast<size_t>(6)) != 6) {
    while (Wire.available()) Wire.read();
    return "Read failed";
  }
  for (uint8_t &value : data) value = Wire.read();
  if (crc8(data, 2) != data[2] || crc8(data + 3, 2) != data[5]) {
    return "CRC error";
  }
  const uint16_t rawT = (uint16_t(data[0]) << 8) | data[1];
  const uint16_t rawH = (uint16_t(data[3]) << 8) | data[4];
  temperature = -45.0f + 175.0f * rawT / 65535.0f;
  humidity = 100.0f * rawH / 65535.0f;
  return nullptr;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 明确指定板载 OLED 引脚
  // 传感器：SDA -> GPIO5, SCL -> GPIO6, VCC -> 3.3V, GND -> GND。
  Wire.begin(OLED_SDA, OLED_SCL, 100000);
  Wire.setTimeOut(50);
  Serial.println("ESP32-C3 72x40 SHT3x monitor; SDA=5 SCL=6");
  for (uint8_t i = 1; i < 127; ++i) {
    if (respondsAt(i)) Serial.printf("I2C found: 0x%02X\n", i);
  }

  uint8_t address = 0;

  if (respondsAt(0x3C)) {
    address = 0x3C;
  } else if (respondsAt(0x3D)) {
    address = 0x3D;
  } else {
    Serial.println("OLED not found. Check SDA/SCL and board version.");
    return;
  }

  Serial.printf("I2C device found: 0x%02X\n", address);

  // U8g2 takes an 8-bit I2C address; both devices share Wire on GPIO5/6.
  oled.setI2CAddress(address << 1);
  oled.setBusClock(100000);
  oled.begin();
  oled.setContrast(128);
  Wire.setTimeOut(50);
  oledReady = true;
}

void loop() {
  const uint32_t now = millis();
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) {
    delay(5);
    return;
  }
  lastSampleMs = now;
  float temperature = 0, humidity = 0;
  const char *error = readSensor(temperature, humidity);
  if (error) {
    Serial.printf("ERROR: %s; retry in 1s\n", error);
    sensorAddress = 0; // 下次重新查找/复位；传感器重新接入后自动恢复。
  } else {
    Serial.printf("SHT3x 0x%02X | Temperature: %.2f C | Humidity: %.2f %%RH\n",
                  sensorAddress, temperature, humidity);
  }
  // OLED 异常时仍继续读取传感器并输出串口。
  if (!oledReady) return;

  oled.clearBuffer();
  if (error) {
    // Short messages fit the 72-pixel width; full diagnostics go to Serial.
    const char *message = "SHT missing";
    if (strcmp(error, "Command failed") == 0) message = "I2C command";
    else if (strcmp(error, "Read failed") == 0) message = "I2C read";
    else if (strcmp(error, "CRC error") == 0) message = "CRC error";
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(0, 10, message);
    oled.drawStr(0, 24, "SDA5 SCL6");
    oled.drawStr(0, 38, "Retry...");
  } else {
    char line[20];
    // 7x14 fixed-width font: even T:-45.0C and H:100.0% fit.
    oled.setFont(u8g2_font_7x14_tf);
    snprintf(line, sizeof(line), "T:%5.1fC", temperature);
    oled.drawStr(0, 12, line);
    snprintf(line, sizeof(line), "H:%5.1f%%", humidity);
    oled.drawStr(0, 26, line);
    oled.drawHLine(0, 30, 72);
    oled.setFont(u8g2_font_5x7_tf);
    oled.drawStr(0, 39, "LIVE 1s");
  }
  oled.sendBuffer();
}
