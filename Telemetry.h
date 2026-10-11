#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <math.h>

namespace telemetry {
inline std::string quote(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 0x20) { char escaped[7]; snprintf(escaped, sizeof escaped, "\\u%04x", c); out += escaped; }
    else out += char(c);
  }
  return out + '"';
}
inline bool validUrl(const std::string &url) {
  if (url.empty()) return true; // Reporting can be configured later.
  size_t start = url.compare(0, 8, "https://") == 0 ? 8 :
                 url.compare(0, 7, "http://") == 0 ? 7 : 0;
  if (!start) return false;
  for (unsigned char c : url) if (c <= 0x20 || c >= 0x7f || c == '#' || c == '\\') return false;
  auto end = url.find_first_of("/?", start);
  std::string authority = url.substr(start, end == std::string::npos ? end : end - start);
  if (authority.empty() || authority.find('@') != std::string::npos) return false;
  // IPv4/hostname and optional numeric port. IPv6 literals are intentionally not supported.
  auto colon = authority.find(':');
  std::string host = authority.substr(0, colon);
  if (host.empty()) return false;
  for (unsigned char c : host)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
  if (colon != std::string::npos) {
    std::string port = authority.substr(colon + 1);
    if (port.empty() || port.size() > 5) return false;
    unsigned n = 0;
    for (char c : port) { if (c < '0' || c > '9') return false; n = n * 10 + c - '0'; }
    if (n == 0 || n > 65535) return false;
  }
  return true;
}
inline bool validToken(const std::string &s) {
  for (unsigned char c : s) if (c <= 0x20 || c >= 0x7f) return false;
  return true;
}
inline bool validThingsBoardBase(const std::string &url) {
  if (url.empty()) return true;
  if (!validUrl(url) || url.find('?') != std::string::npos) return false;
  auto start = url.find("://") + 3;
  auto slash = url.find('/', start);
  return slash == std::string::npos || slash == url.size() - 1;
}
inline std::string pathSegment(const std::string &s) {
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
    else { char encoded[4]; snprintf(encoded, sizeof encoded, "%%%02X", c); out += encoded; }
  }
  return out;
}
inline std::string reportUrl(bool thingsboard, std::string url, const std::string &token) {
  if (!thingsboard || url.empty()) return url;
  if (url.back() == '/') url.pop_back();
  return url + "/api/v1/" + pathSegment(token) + "/telemetry";
}
inline bool interval(const std::string &s, uint32_t &result) {
  if (s.empty() || s.size() > 5) return false;
  uint32_t n = 0;
  for (char c : s) { if (c < '0' || c > '9') return false; n = n * 10 + c - '0'; }
  if (n < 10 || n > 86400) return false;
  result = n; return true;
}
inline bool fresh(bool valid, uint32_t sampledMs, uint32_t now) {
  return valid && uint32_t(now - sampledMs) <= 3000;
}
inline bool due(uint32_t now, uint32_t last, uint32_t wait) {
  return uint32_t(now - last) >= wait;
}
inline uint32_t retryMs(unsigned failures) {
  unsigned shift = failures > 5 ? 4 : failures ? failures - 1 : 0;
  return 15000UL << shift; // 15, 30, 60, 120, 240 seconds.
}
inline std::string payload(const char *id, const char *room, float temperature,
                           float humidity, const char *sampledAt) {
  if (!isfinite(temperature) || !isfinite(humidity)) return "";
  char values[96];
  snprintf(values, sizeof values, ",\"temperature\":%.2f,\"humidity\":%.2f", temperature, humidity);
  std::string out = "{\"device_id\":" + quote(id) + ",\"room\":" + quote(room) + values;
  if (sampledAt && *sampledAt) out += ",\"sampled_at\":" + quote(sampledAt);
  return out + '}';
}
inline std::string thingsBoardPayload(const char *id, const char *room, float temperature,
                                      float humidity, int64_t sampledMs) {
  auto values = payload(id, room, temperature, humidity, "");
  if (values.empty() || sampledMs < 1700000000000LL) return values;
  return "{\"ts\":" + std::to_string(sampledMs) + ",\"values\":" + values + "}";
}
}
