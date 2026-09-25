#pragma once

#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class AnkiSettingsActivity final : public Activity {
 public:
  explicit AnkiSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("AnkiSettings", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // URL, discover, token, handedness, font, orientation, max/deck, max/total, card font, clear
  static constexpr int MENU_ITEMS = 10;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  std::string statusMessage;
  bool wifiActivated = false;

  void handleSelection();
  void saveConfig();
  void startDiscover();
  void runDiscover();
};
