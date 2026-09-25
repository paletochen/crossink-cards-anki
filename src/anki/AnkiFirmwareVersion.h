#pragma once

#include <cstdio>
#include <cstring>

// xteink-anki package version (bump with RELEASE_NOTES / dist binary).
// CROSSPOINT_VERSION is set by PlatformIO (e.g. 1.4.1-anki-2.5.7 on gh_release).
#ifndef XTEINK_ANKI_FW_VERSION
#define XTEINK_ANKI_FW_VERSION "2.5.7"
#endif

// Label for Anki UI headers (static buffer — UI thread only).
inline const char* ankiFirmwareVersionLabel() {
  static char buf[72];
#ifndef CROSSPOINT_VERSION
  return "anki-" XTEINK_ANKI_FW_VERSION;
#else
  // Release builds already tag CROSSPOINT_VERSION with "-anki-…".
  if (std::strstr(CROSSPOINT_VERSION, "anki") != nullptr) {
    return CROSSPOINT_VERSION;
  }
  // Dev / stock: show both CrossPoint base and Anki package version.
  std::snprintf(buf, sizeof(buf), "%s · anki-%s", CROSSPOINT_VERSION, XTEINK_ANKI_FW_VERSION);
  return buf;
#endif
}
