#pragma once
#include "Telemetry.h"
#include <cstring>
namespace configuration {
constexpr uint32_t CONFIG_MAGIC = 0x43335402; // Schema version 2.
constexpr uint32_t LEGACY_MAGIC = 0x43335401;
struct LegacyConfig {
  uint32_t magic = CONFIG_MAGIC;
  uint32_t seconds = 60;
  char ssid[33] = {};
  char password[64] = {};
  char room[64] = {};
  char url[257] = {};
  char token[257] = {};
};
struct Config : LegacyConfig { uint32_t thingsboard = 0; };
static_assert(sizeof(LegacyConfig) == 684, "Preserve deployed v1 NVS layout");
static_assert(sizeof(Config) == 688, "Preserve v2 NVS layout");
inline bool configValid(const Config &c) {
  if (c.magic != CONFIG_MAGIC || c.seconds < 10 || c.seconds > 86400) return false;
  if (!memchr(c.ssid, 0, sizeof c.ssid) || !memchr(c.password, 0, sizeof c.password) ||
      !memchr(c.room, 0, sizeof c.room) || !memchr(c.url, 0, sizeof c.url) ||
      !memchr(c.token, 0, sizeof c.token)) return false;
  return c.thingsboard <= 1 && telemetry::validUrl(c.url) && telemetry::validToken(c.token) &&
         (!c.thingsboard || (telemetry::validThingsBoardBase(c.url) && (!*c.url || *c.token))) &&
         (!*c.password || strlen(c.password) >= 8);
}

inline bool decode(const void *data, size_t length, Config &result) {
  Config saved;
  if (length == sizeof(Config)) memcpy(&saved, data, length);
  else if (length == sizeof(LegacyConfig)) {
    LegacyConfig legacy;
    memcpy(&legacy, data, length);
    if (legacy.magic != LEGACY_MAGIC) return false;
    static_cast<LegacyConfig &>(saved) = legacy;
    saved.magic = CONFIG_MAGIC;
    saved.thingsboard = 0;
  } else return false;
  if (!configValid(saved)) return false;
  result = saved;
  return true;
}
} // namespace configuration
