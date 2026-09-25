#pragma once

#include <string>

// LAN discovery for the Mac Anki add-on (_xteink-anki._tcp).
// Requires an active Wi-Fi STA connection. Token is never read from mDNS.
namespace AnkiMdns {

// On success writes http://A.B.C.D:port into urlOut and returns true.
// On failure urlOut is unchanged and error receives a short reason (optional).
bool discoverServerUrl(std::string& urlOut, std::string* error = nullptr);

}  // namespace AnkiMdns
