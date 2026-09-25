#include "AnkiMdns.h"

#include <ESPmDNS.h>
#include <WiFi.h>

#include <cstdio>

namespace AnkiMdns {

bool discoverServerUrl(std::string& urlOut, std::string* error) {
  if (WiFi.status() != WL_CONNECTED) {
    if (error) *error = "wifi";
    return false;
  }

  MDNS.end();
  if (!MDNS.begin("xteink-anki-client")) {
    if (error) *error = "mdns";
    return false;
  }

  // Mac advertises _xteink-anki._tcp → queryService("xteink-anki", "tcp")
  const int found = MDNS.queryService("xteink-anki", "tcp");
  if (found <= 0) {
    MDNS.end();
    if (error) *error = "none";
    return false;
  }

  std::string url;
  for (int i = 0; i < found; i++) {
    const IPAddress ip = MDNS.address(i);
    const uint16_t port = MDNS.port(i);
    if (port == 0) continue;
    if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) continue;
    char buf[64];
    snprintf(buf, sizeof(buf), "http://%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3],
             static_cast<unsigned>(port));
    url = buf;
    break;
  }
  MDNS.end();

  if (url.empty()) {
    if (error) *error = "none";
    return false;
  }
  urlOut = std::move(url);
  if (error) error->clear();
  return true;
}

}  // namespace AnkiMdns
