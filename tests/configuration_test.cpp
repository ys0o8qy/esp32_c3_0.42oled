#include "../Configuration.h"
#include <cassert>
int main() {
  using namespace configuration;
  LegacyConfig old;
  old.magic = LEGACY_MAGIC;
  strcpy(old.ssid, "phone-network");
  strcpy(old.password, "phone-password");
  strcpy(old.room, "bedroom");
  Config loaded;
  strcpy(loaded.ssid, "embedded-network");
  strcpy(loaded.url, "https://embedded.example");
  assert(decode(&old, sizeof old, loaded));
  assert(!strcmp(loaded.ssid, "phone-network"));
  assert(!*loaded.url); // A saved empty URL must override an embedded URL.
  assert(loaded.magic == CONFIG_MAGIC && loaded.thingsboard == 0);
  auto current = loaded;
  current.thingsboard = 1;
  strcpy(current.url, "https://tb.example:443/");
  strcpy(current.token, "device-token");
  assert(decode(&current, sizeof current, loaded));
  assert(loaded.thingsboard == 1 && !strcmp(loaded.token, "device-token"));
  current.token[0] = 0;
  assert(!decode(&current, sizeof current, loaded));
  assert(!strcmp(loaded.token, "device-token")); // Failed load doesn't modify output.
  old.magic = 0;
  assert(!decode(&old, sizeof old, loaded));
  old.magic = LEGACY_MAGIC;
  memset(old.ssid, 'x', sizeof old.ssid);
  assert(!decode(&old, sizeof old, loaded));
  assert(!decode(&old, sizeof old - 1, loaded));
}
