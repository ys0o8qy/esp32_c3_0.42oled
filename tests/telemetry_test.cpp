#include "../Telemetry.h"
#include <cassert>
#include <iostream>
int main() {
  using namespace telemetry;
  uint32_t n=0;
  assert(interval("60", n) && n==60);
  assert(interval("86400", n) && !interval("86401", n));
  assert(!interval("0", n) && !interval("-1", n) && !interval("60junk", n));
  assert(!interval("10.5", n) && !interval("4294967296", n));
  assert(validUrl("") && validUrl("https://example.org:8443/v1?x=1"));
  assert(validUrl("http://192.168.1.10:8080/readings"));
  assert(!validUrl("http://") && !validUrl("ftp://example.org"));
  assert(!validUrl("http://user:pass@host/") && !validUrl("https://host/#x"));
  assert(!validUrl("https://host\r\nX:evil") && !validUrl("http://host:65536/"));
  assert(!validUrl("http://host:abc") && !validUrl("http://host\\evil"));
  assert(validToken("abc-_.123") && !validToken("abc\r\nX:evil"));
  assert(fresh(true, 0xfffffff0U, 10) && !fresh(false, 10, 10));
  assert(!fresh(true, 100, 3101) && fresh(true, 100, 3100));
  assert(due(20, 0xfffffff0U, 30) && !due(20, 0xfffffff0U, 40));
  assert(retryMs(1)==15000 && retryMs(2)==30000 && retryMs(99)==240000);
  auto body=payload("c3-test", "卧室\"\n\\", 23.5, 60.25, "2026-10-10T00:00:00Z");
  assert(body.find("\\\"\\u000a\\\\") != std::string::npos);
  assert(payload("id", "room", NAN, 50, "").empty());
  assert(payload("id", "room", 20, 50, "").find("sampled_at")==std::string::npos);
  std::cout << body << std::endl;
}
