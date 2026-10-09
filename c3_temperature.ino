#include <Wire.h>
#include <SensirionI2cSht3x.h>
#include <U8g2lib.h>
#include "NetworkService.h"

constexpr int OLED_SDA = 5;
constexpr int OLED_SCL = 6;
constexpr int SETUP_BUTTON_PIN = 9; // Hold for 3s while running (not during power-on).
uint32_t buttonSince = 0;
bool buttonHeld = false;
bool buttonHandled = false;

void handleSetupButton() {
  if (digitalRead(SETUP_BUTTON_PIN) == LOW) {
    if (!buttonHeld) { buttonHeld = true; buttonSince = millis(); }
    if (!buttonHandled && uint32_t(millis() - buttonSince) >= 3000) {
      buttonHandled = true;
      networkRequestPortal();
    }
  } else { buttonHeld = false; buttonHandled = false; }
}

// 0.42-inch OLED: 72x40 pixels; the driver handles the controller offset.
// Change U8G2_R0 to U8G2_R2 only if the physical display is upside down.
U8G2_SSD1306_72X40_ER_F_HW_I2C oled(
    U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
bool oledReady = false;
SensirionI2cSht3x sensor;
uint8_t sensorAddress = 0;
uint32_t lastSampleMs = 0;
uint32_t successfulSamples = 0;
constexpr uint32_t SAMPLE_INTERVAL_MS = 1000;

// Detailed driver errors (including CRC failures) are sent to Serial.
bool sensorCommandOk(int16_t result, const char *operation) {
  if (result == NO_ERROR) return true;
  char message[128];
  errorToString(result, message, sizeof(message));
  Serial.printf("SHT3x 0x%02X %s: %s (code=%d)\n",
                sensorAddress, operation, message, result);
  return false;
}

bool respondsAt(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

const char *connectSensor() {
  sensorAddress = respondsAt(0x44) ? 0x44 : (respondsAt(0x45) ? 0x45 : 0);
  if (!sensorAddress) return "SHT3x missing";
  sensor.begin(Wire, sensorAddress);
  if (!sensorCommandOk(sensor.stopMeasurement(), "stopMeasurement")) {
    return "Init failed";
  }
  if (!sensorCommandOk(sensor.softReset(), "softReset")) return "Init failed";
  if (!sensorCommandOk(sensor.disableHeater(), "disableHeater")) {
    return "Init failed";
  }
  // The official library handles command delays, including heater settling.
  return nullptr;
}

const char *readSensor(float &temperature, float &humidity) {
  if (!sensorAddress) {
    const char *error = connectSensor();
    if (error) return error;
  }
  // High repeatability, no clock stretching. CRC and conversion are in the library.
  if (!sensorCommandOk(sensor.measureSingleShot(REPEATABILITY_HIGH, false,
                                                temperature, humidity),
                       "measureSingleShot")) {
    return "Read failed";
  }
  return nullptr;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(SETUP_BUTTON_PIN, INPUT_PULLUP);
  networkBegin();

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
  handleSetupButton();
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
    ++successfulSamples;
    Serial.printf("SHT3x 0x%02X | Temperature: %.2f C | Humidity: %.2f %%RH\n",
                  sensorAddress, temperature, humidity);
  }
  networkPublishSample(temperature, humidity, error == nullptr);
  // OLED 异常时仍继续读取传感器并输出串口。
  if (!oledReady) return;

  oled.clearBuffer();
  const NetworkView net = networkView();
  if (net.portalActive && millis() % 8000 < 6000) {
    // Alternate setup instructions with readings; sampling always continues.
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(0, 8, "WiFi setup");
    oled.drawStr(0, 18, net.apName);
    char password[16];
    snprintf(password, sizeof password, "P:%s", net.apPassword);
    oled.drawStr(0, 28, password);
    oled.drawStr(0, 39, "192.168.4.1");
    oled.sendBuffer();
    return;
  }
  if (error) {
    // Short messages fit the 72-pixel width; full diagnostics go to Serial.
    const char *message = "SHT missing";
    if (strcmp(error, "Init failed") == 0) message = "SHT init";
    else if (strcmp(error, "Read failed") == 0) message = "I2C read";
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
    // Advance only after a successful measurement, even if values are unchanged.
    snprintf(line, sizeof(line), "LIVE %06lu",
             static_cast<unsigned long>(successfulSamples % 1000000UL));
    if ((millis() / 3000) % 2) {
      if (net.portalActive) snprintf(line, sizeof line, "SETUP AP");
      else if (!net.connected) snprintf(line, sizeof line, "WiFi offline");
      else if (!net.configured) snprintf(line, sizeof line, "API not set");
      else if (strcmp(net.state, "SENDING") == 0) snprintf(line, sizeof line, "Sending...");
      else if (strcmp(net.state, "WAIT_CLOCK") == 0) snprintf(line, sizeof line, "Sync clock");
      else if (strcmp(net.state, "RETRY") == 0) snprintf(line, sizeof line, "Retry %d", net.httpStatus);
      else snprintf(line, sizeof line, "UP %lu WiFi+", static_cast<unsigned long>(net.uploaded % 10000));
    }
    oled.drawStr(0, 39, line);
    if (successfulSamples & 1U) oled.drawBox(67, 34, 3, 3);
  }
  oled.sendBuffer();
}
