#include "NetworkService.h"
#include "Telemetry.h"
#include "BuildDefaults.h"
#include "Configuration.h"
#include "PortalPage.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <time.h>
#include <sys/time.h>

namespace {
using namespace configuration;
constexpr uint32_t PORTAL_TIMEOUT_MS = 10 * 60 * 1000;
struct Sample { float temperature; float humidity; bool valid; uint32_t ms; int64_t utcMs; };
Config config; // Used only by the portal task after initialization.
char deviceId[20];
char apName[16];
char apPassword[12];
char csrf[33];
QueueHandle_t samples = nullptr;
QueueHandle_t configs = nullptr;
TaskHandle_t portalTaskHandle = nullptr;
portMUX_TYPE viewMutex = portMUX_INITIALIZER_UNLOCKED;
NetworkView view = {};
WebServer server(80);
DNSServer dns;
bool portalActive = false;
uint32_t portalStartedMs = 0;
bool closing = false;
uint32_t closeStartedMs = 0;

void state(const char *text, int code = 0) {
  portENTER_CRITICAL(&viewMutex);
  strlcpy(view.state, text, sizeof(view.state));
  view.httpStatus = code;
  portEXIT_CRITICAL(&viewMutex);
}
bool loadConfig() {
  Preferences prefs;
  if (!prefs.begin("room-sensor", true)) return false;
  Config saved;
  size_t length = prefs.getBytesLength("config");
  uint8_t bytes[sizeof(Config)];
  bool ok = length <= sizeof bytes && prefs.getBytes("config", bytes, length) == length &&
            decode(bytes, length, saved);
  prefs.end();
  if (ok) config = saved;
  return ok;
}
bool saveConfig(const Config &next) {
  Preferences prefs;
  if (!prefs.begin("room-sensor", false)) return false;
  bool ok = prefs.putBytes("config", &next, sizeof next) == sizeof next;
  prefs.end();
  return ok;
}
std::string quote(const char *s) { return telemetry::quote(s); }
void json(int code, const std::string &body) {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.send(code, "application/json; charset=utf-8", body.c_str());
}
void errorResponse(const char *message) { json(400, "{\"error\":" + quote(message) + "}"); }
bool portalRequest(bool mutation = false) {
  // Never expose the configuration endpoint on the home's station interface.
  if (!portalActive || server.client().localIP() != WiFi.softAPIP()) {
    json(403, "{\"error\":\"请连接设备配置热点\"}"); return false;
  }
  if (mutation && (closing || server.header("X-Setup-Token") != csrf)) {
    json(403, "{\"error\":\"配置会话已失效，请刷新页面\"}"); return false;
  }
  return true;
}
void getConfig() {
  if (!portalRequest()) return;
  // Passwords and tokens are never returned to the browser.
  std::string body = "{\"device_id\":" + quote(deviceId) + ",\"csrf\":" + quote(csrf) +
      ",\"ssid\":" + quote(config.ssid) + ",\"room\":" + quote(config.room) +
      ",\"mode\":" + quote(config.thingsboard ? "thingsboard" : "http") +
      ",\"url\":" + quote(config.url) + ",\"seconds\":" + std::to_string(config.seconds) +
      ",\"password_set\":" + (*config.password ? "true" : "false") +
      ",\"token_set\":" + (*config.token ? "true" : "false") +
      ",\"open_wifi\":" + (*config.ssid && !*config.password ? "true" : "false") + "}";
  json(200, body);
}
void updateConfig() {
  if (!portalRequest(true)) return;
  if (!server.hasArg("ssid") || !server.hasArg("room") || !server.hasArg("url") || !server.hasArg("seconds")) {
    errorResponse("配置字段不完整"); return;
  }
  String ssid = server.arg("ssid"), room = server.arg("room"), url = server.arg("url");
  String password = server.arg("password"), token = server.arg("token");
  room.trim(); url.trim();
  uint32_t seconds;
  if (ssid.length() == 0 || ssid.length() >= sizeof config.ssid) { errorResponse("Wi-Fi 名称需要 1–32 字节"); return; }
  if (room.length() == 0 || room.length() >= sizeof config.room) { errorResponse("房间名称需要 1–63 字节（中文通常每字 3 字节）"); return; }
  if (url.length() >= sizeof config.url || !telemetry::validUrl(url.c_str())) { errorResponse("接口地址需为 HTTP/HTTPS URL，最长 256 字节，不支持账号密码或 IPv6 地址"); return; }
  if (!telemetry::interval(server.arg("seconds").c_str(), seconds)) { errorResponse("上报间隔需为 10–86400 的整数秒"); return; }
  if (token.length() >= sizeof config.token || !telemetry::validToken(token.c_str())) { errorResponse("Token 最长 256 字节，不能包含空白或非 ASCII 字符"); return; }
  Config next = config;
  String mode = server.arg("mode");
  if (mode != "http" && mode != "thingsboard") { errorResponse("请选择上报模式"); return; }
  next.thingsboard = mode == "thingsboard";
  if (next.thingsboard != config.thingsboard) next.token[0] = 0;
  if (next.thingsboard && !telemetry::validThingsBoardBase(url.c_str())) {
    errorResponse("ThingsBoard 地址只填写 http(s)://主机:端口，不包含接口路径或查询参数"); return;
  }
  bool openWifi = server.hasArg("open_wifi");
  if (openWifi) next.password[0] = 0;
  else if (password.length()) {
    if (password.length() < 8 || password.length() >= sizeof next.password) { errorResponse("Wi-Fi 密码需要 8–63 字节"); return; }
    strlcpy(next.password, password.c_str(), sizeof next.password);
  } else if (ssid != config.ssid || !*config.password) {
    errorResponse("请填写 Wi-Fi 密码，或勾选无密码网络"); return;
  }
  if (server.hasArg("clear_token")) next.token[0] = 0;
  else if (token.length()) strlcpy(next.token, token.c_str(), sizeof next.token);
  strlcpy(next.ssid, ssid.c_str(), sizeof next.ssid);
  strlcpy(next.room, room.c_str(), sizeof next.room);
  strlcpy(next.url, url.c_str(), sizeof next.url);
  next.seconds = seconds;
  if (next.thingsboard && *next.url && !*next.token) {
    errorResponse("ThingsBoard 需要 Device Access Token；切换模式后请重新填写"); return;
  }
  if (!saveConfig(next)) { state("NVS_ERROR"); json(500, "{\"error\":\"保存失败，原配置未应用，请重试\"}"); return; }
  config = next;
  portalStartedMs = millis();
  xQueueOverwrite(configs, &next);
  json(200, "{\"ok\":true}");
}
void getStatus() {
  if (!portalRequest()) return;
  auto v = networkView();
  json(200, "{\"connected\":" + std::string(v.connected ? "true" : "false") +
      ",\"state\":" + quote(v.state) + ",\"uploaded\":" + std::to_string(v.uploaded) +
      ",\"http_status\":" + std::to_string(v.httpStatus) + "}");
}
void startPortal() {
  if (portalActive) { portalStartedMs = millis(); return; }
  snprintf(apPassword, sizeof apPassword, "%08lX", static_cast<unsigned long>(esp_random()));
  snprintf(csrf, sizeof csrf, "%08lX%08lX%08lX%08lX", static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  if (!WiFi.softAP(apName, apPassword, 1, false, 2)) { Serial.println("Config AP failed"); return; }
  portalActive = true;
  closing = false;
  portalStartedMs = millis();
  dns.start(53, "*", WiFi.softAPIP());
  server.begin();
  portENTER_CRITICAL(&viewMutex);
  view.portalActive = true;
  strlcpy(view.apName, apName, sizeof view.apName);
  strlcpy(view.apPassword, apPassword, sizeof view.apPassword);
  portEXIT_CRITICAL(&viewMutex);
  Serial.printf("Config AP: %s; password shown on OLED; http://192.168.4.1\n", apName);
}
void stopPortal() {
  server.stop(); dns.stop(); WiFi.softAPdisconnect(true);
  portalActive = false; closing = false;
  portENTER_CRITICAL(&viewMutex);
  view.portalActive = false;
  memset(view.apPassword, 0, sizeof view.apPassword);
  portEXIT_CRITICAL(&viewMutex);
}
void portalTask(void *) {
  const char *headers[] = {"X-Setup-Token"};
  server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, [] {
    if (!portalRequest()) return;
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Frame-Options", "DENY");
    server.send_P(200, "text/html; charset=utf-8", PORTAL_PAGE);
  });
  server.on("/api/config", HTTP_GET, getConfig);
  server.on("/api/config", HTTP_POST, updateConfig);
  server.on("/api/status", HTTP_GET, getStatus);
  server.on("/api/close", HTTP_POST, [] {
    if (!portalRequest(true)) return;
    json(200, "{\"ok\":true}"); closing = true; closeStartedMs = millis();
  });
  server.onNotFound([] {
    if (!portalRequest()) return;
    server.sendHeader("Location", "http://192.168.4.1/"); server.send(302, "text/plain", "Open setup");
  });
  if (!*config.ssid) startPortal();
  for (;;) {
    if (ulTaskNotifyTake(pdTRUE, 0)) startPortal();
    if (portalActive) {
      dns.processNextRequest(); server.handleClient();
      if ((closing && telemetry::due(millis(), closeStartedMs, 1500)) ||
          (*config.ssid && telemetry::due(millis(), portalStartedMs, PORTAL_TIMEOUT_MS))) stopPortal();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
int postSample(const Config &active, const Sample &sample) {
  char timestamp[24] = {};
  if (sample.utcMs >= 1700000000000LL) {
    time_t seconds = sample.utcMs / 1000;
    struct tm utc;
    gmtime_r(&seconds, &utc);
    strftime(timestamp, sizeof timestamp, "%Y-%m-%dT%H:%M:%SZ", &utc);
  }
  std::string body = active.thingsboard
      ? telemetry::thingsBoardPayload(deviceId, active.room, sample.temperature, sample.humidity, sample.utcMs)
      : telemetry::payload(deviceId, active.room, sample.temperature, sample.humidity, timestamp);
  std::string url = telemetry::reportUrl(active.thingsboard, active.url, active.token);
  if (body.empty()) return -1;
  esp_http_client_config_t options = {};
  options.url = url.c_str();
  options.method = HTTP_METHOD_POST;
  options.timeout_ms = 5000;
  options.disable_auto_redirect = true;
  options.crt_bundle_attach = esp_crt_bundle_attach;
  options.buffer_size = 512;
  options.buffer_size_tx = 2048; // Encoded ThingsBoard token plus URL and headers.
  auto client = esp_http_client_init(&options);
  if (!client) return -1;
  esp_http_client_set_header(client, "Content-Type", "application/json");
  std::string authorization;
  if (!active.thingsboard && *active.token) {
    authorization = std::string("Bearer ") + active.token;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
  }
  esp_http_client_set_post_field(client, body.c_str(), body.size());
  esp_err_t result = esp_http_client_perform(client);
  int status = result == ESP_OK ? esp_http_client_get_status_code(client) : -1;
  esp_http_client_cleanup(client);
  return status;
}
void reporterTask(void *) {
  Config active;
  uint32_t lastConnect = millis() - 30000UL;
  uint32_t lastAttempt = 0, waitMs = 0;
  unsigned failures = 0;
  bool wasConnected = false;
  int lastHttpCode = 0;
  for (;;) {
    Config next;
    if (xQueueReceive(configs, &next, 0) == pdTRUE) {
      bool wifiChanged = strcmp(active.ssid, next.ssid) || strcmp(active.password, next.password);
      active = next;
      if (wifiChanged) { WiFi.disconnect(false, false); lastConnect = millis() - 30000UL; }
      waitMs = 0; failures = 0; lastHttpCode = 0;
    }
    bool connected = WiFi.status() == WL_CONNECTED;
    portENTER_CRITICAL(&viewMutex);
    view.connected = connected;
    view.configured = *active.url != 0;
    portEXIT_CRITICAL(&viewMutex);
    if (!*active.ssid) state("LOCAL");
    else if (!connected) {
      state("WAIT_WIFI");
      if (telemetry::due(millis(), lastConnect, 30000)) {
        lastConnect = millis(); WiFi.begin(active.ssid, active.password);
      }
    } else {
      if (!wasConnected) {
        configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "ntp.aliyun.com");
        waitMs = 0; failures = 0;
      }
      Sample sample = {};
      bool latest = xQueuePeek(samples, &sample, 0) == pdTRUE && telemetry::fresh(sample.valid, sample.ms, millis());
      if (!*active.url) state("NO_URL");
      else if (!latest) state("WAIT_SAMPLE");
      else if (strncmp(active.url, "https://", 8) == 0 && time(nullptr) < 1700000000) state("WAIT_CLOCK");
      else if (telemetry::due(millis(), lastAttempt, waitMs)) {
        state("SENDING");
        int code = postSample(active, sample);
        lastHttpCode = code;
        lastAttempt = millis();
        if (code >= 200 && code < 300) {
          failures = 0; waitMs = active.seconds * 1000UL;
          portENTER_CRITICAL(&viewMutex); ++view.uploaded; portEXIT_CRITICAL(&viewMutex);
          state("OK", code);
        } else {
          if (failures < 5) ++failures;
          waitMs = telemetry::retryMs(failures);
          state("RETRY", code);
        }
        Serial.printf("Report HTTP=%d; next attempt in %lu s\n", code, static_cast<unsigned long>(waitMs / 1000));
      } else {
        state(failures ? "RETRY" : lastHttpCode ? "OK" : "READY", lastHttpCode);
      }
    }
    wasConnected = connected;
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}
} // namespace

NetworkView networkView() {
  portENTER_CRITICAL(&viewMutex); NetworkView copy = view; portEXIT_CRITICAL(&viewMutex);
  return copy;
}
bool networkBegin() {
  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(deviceId, sizeof deviceId, "c3-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(apName, sizeof apName, "C3-%02X%02X%02X", mac[3], mac[4], mac[5]);
  // A saved phone configuration is authoritative as a whole (including blanks).
  // Embedded defaults are not persisted, so a rebuild can update them on an unconfigured device.
  if (!loadConfig()) {
    strlcpy(config.ssid, C3_WIFI_SSID, sizeof config.ssid);
    strlcpy(config.password, C3_WIFI_PASSWORD, sizeof config.password);
    strlcpy(config.room, C3_ROOM_NAME, sizeof config.room);
    strlcpy(config.url, C3_REPORT_URL, sizeof config.url);
    strlcpy(config.token, C3_REPORT_TOKEN, sizeof config.token);
    config.seconds = C3_REPORT_SECONDS;
    config.thingsboard = C3_THINGSBOARD;
    if (!configValid(config)) { config = Config{}; Serial.println("Invalid build defaults; using phone setup"); }
    else Serial.println("Configuration source: firmware defaults");
  } else Serial.println("Configuration source: saved phone settings");
  if (!*config.room) strlcpy(config.room, deviceId, sizeof config.room);
  WiFi.persistent(false);
  WiFi.setHostname(deviceId);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  samples = xQueueCreate(1, sizeof(Sample));
  configs = xQueueCreate(1, sizeof(Config));
  if (!samples || !configs) { state("TASK_ERROR"); return false; }
  xQueueOverwrite(configs, &config);
  if (xTaskCreate(reporterTask, "reporter", 10240, nullptr, 1, nullptr) != pdPASS ||
      xTaskCreate(portalTask, "portal", 8192, nullptr, 1, &portalTaskHandle) != pdPASS) {
    state("TASK_ERROR"); return false;
  }
  return true;
}
void networkRequestPortal() { if (portalTaskHandle) xTaskNotifyGive(portalTaskHandle); }
void networkPublishSample(float temperature, float humidity, bool valid) {
  if (!samples) return;
  struct timeval now;
  gettimeofday(&now, nullptr);
  Sample sample = {temperature, humidity, valid, millis(), int64_t(now.tv_sec) * 1000 + now.tv_usec / 1000};
  xQueueOverwrite(samples, &sample);
}
