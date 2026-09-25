#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "anki/AnkiStore.h"
#include "util/ButtonNavigator.h"

class AnkiActivity final : public Activity {
 public:
  enum class MenuAction { LEARN, DECK, DOWNLOAD, UPLOAD, CONFIGURE };

  explicit AnkiActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Anki", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class State { MENU, WORKING, QUESTION, ANSWER, MESSAGE, ERROR };
  enum class NetworkAction { NONE, PULL, PUSH };

  struct MenuItem {
    MenuAction action = MenuAction::LEARN;
    uint16_t deckIndex = 0;
  };

  State state = State::MENU;
  NetworkAction networkAction = NetworkAction::NONE;
  ButtonNavigator buttonNavigator;
  std::vector<MenuItem> menuItems;
  int selectedIndex = 0;
  AnkiCard currentCard;
  std::string message;
  std::string detail;
  bool restartOnDismiss = false;
  bool wifiActivated = false;
  // One mDNS offer per pull/push attempt (avoid dialog loop after retry fails).
  bool discoverOfferedThisAction = false;
  uint32_t cardStartedAt = 0;

  size_t pageStart = 0;
  size_t nextPageStart = 0;
  std::vector<size_t> pageHistory;
  // E-ink ghost cleanup: same cadence as the reader (SETTINGS refresh frequency).
  // Shared by card + menu paints; 0 forces HALF on the next paint, then FAST.
  int pagesUntilFullRefresh = 0;

  // Side Up/Down share one ADC — cannot chord both at once. Flag gestures:
  // Confirm+side, or long-press side (short press still pages on release).
  static constexpr unsigned long kFlagHoldMs = 550;
  int8_t pendingSidePageDir = 0;  // -1 prev, +1 next, 0 none
  bool flagHoldFired = false;

  void rebuildMenu();
  void displayCardBuffer();
  void displayUiBuffer();
  void handleMenuSelection();
  void startNetworkAction(NetworkAction action);
  void onWifiSelectionComplete(bool success);
  void performNetworkAction();
  // After NETWORK_ERROR: offer mDNS search vs "Anki is open / keep URL".
  void offerDiscoverAfterConnectFailure();
  void onDiscoverOfferResult(bool search);
  void reconnectWifiThenDiscoverAndRetry();
  void discoverThenRetry();
  void startReview();
  void startReviewForDeck(uint16_t deckIndex);
  void showAnswer();
  void grade(uint8_t ease);
  bool loadCurrentCard();
  void resetPaging();
  void handleCardPaging();
  // Card side-button handling: flag gestures + short-press paging.
  bool handleCardSideButtons();
  bool tryToggleFlagGesture();
  bool applyFlagToggle();
  void nextPage();
  void previousPage();
  void applyUiOrientation(bool forCard);

  void renderMenu();
  void renderCard(bool answer);
  void renderMessage(bool error);
  size_t drawTextPage(const std::string& text, size_t start, int x, int y, int width, int maxLines, int lineHeight,
                      int scale, bool bold);
};
