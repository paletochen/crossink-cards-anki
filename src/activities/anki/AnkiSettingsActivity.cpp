#include "AnkiSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <cstdio>
#include <variant>

#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "anki/AnkiMdns.h"
#include "anki/AnkiStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "../../anki/AnkiFirmwareVersion.h"

namespace {
const StrId MENU_NAMES[10] = {
    StrId::STR_ANKI_SERVER_URL,  StrId::STR_ANKI_DISCOVER,      StrId::STR_ANKI_API_TOKEN,
    StrId::STR_ANKI_HANDEDNESS,  StrId::STR_ANKI_FONT_SIZE,     StrId::STR_ANKI_ORIENTATION,
    StrId::STR_ANKI_MAX_PER_DECK, StrId::STR_ANKI_MAX_TOTAL,    StrId::STR_ANKI_CARD_FONT,
    StrId::STR_ANKI_CLEAR_LOCAL,
};

const char* fontScaleLabel(const uint8_t scale) {
  switch (scale) {
    case 1:
      return tr(STR_ANKI_FONT_SMALL);
    case 3:
      return tr(STR_ANKI_FONT_LARGE);
    case 2:
    default:
      return tr(STR_ANKI_FONT_MEDIUM);
  }
}

std::string formatCount(const uint16_t value) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u", static_cast<unsigned int>(value));
  return std::string(buf);
}
}  // namespace

void AnkiSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  statusMessage.clear();
  wifiActivated = false;
  std::string error;
  if (!ANKI_STORE.load(error)) statusMessage = error;
  requestUpdate();
}

void AnkiSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, MENU_ITEMS);
    statusMessage.clear();
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, MENU_ITEMS);
    statusMessage.clear();
    requestUpdate();
  });
}

void AnkiSettingsActivity::saveConfig() {
  std::string error;
  statusMessage = ANKI_STORE.saveConfig(error) ? "" : error;
  requestUpdate();
}

void AnkiSettingsActivity::startDiscover() {
  statusMessage = tr(STR_ANKI_DISCOVERING);
  requestUpdate();

  if (WiFi.status() == WL_CONNECTED) {
    runDiscover();
    return;
  }

  wifiActivated = true;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                             statusMessage = tr(STR_ANKI_DISCOVER_WIFI);
                             requestUpdate();
                             return;
                           }
                           runDiscover();
                         });
}

void AnkiSettingsActivity::runDiscover() {
  statusMessage = tr(STR_ANKI_DISCOVERING);
  requestUpdate();

  std::string url;
  if (!AnkiMdns::discoverServerUrl(url)) {
    statusMessage = tr(STR_ANKI_DISCOVER_NONE);
    requestUpdate();
    return;
  }

  ANKI_STORE.setServerUrl(url);
  std::string error;
  if (!ANKI_STORE.saveConfig(error)) {
    statusMessage = error;
  } else {
    statusMessage = std::string(tr(STR_ANKI_DISCOVER_OK)) + ": " + url;
  }
  requestUpdate();
}

void AnkiSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    const std::string current = ANKI_STORE.getServerUrl();
    const std::string initial = current.empty() ? "http://192.168." : current;
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ANKI_SERVER_URL), initial, 160,
                                                InputType::Url),
        [this](const ActivityResult& result) {
          if (result.isCancelled) return;
          const auto& keyboard = std::get<KeyboardResult>(result.data);
          ANKI_STORE.setServerUrl(keyboard.text == "http://" || keyboard.text == "https://" ? "" : keyboard.text);
          saveConfig();
        });
    return;
  }

  if (selectedIndex == 1) {
    startDiscover();
    return;
  }

  if (selectedIndex == 2) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ANKI_API_TOKEN),
                                                                   ANKI_STORE.getApiToken(), 160, InputType::Password),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) return;
                             const auto& keyboard = std::get<KeyboardResult>(result.data);
                             ANKI_STORE.setApiToken(keyboard.text);
                             saveConfig();
                           });
    return;
  }

  if (selectedIndex == 3) {
    ANKI_STORE.setLeftHanded(!ANKI_STORE.isLeftHanded());
    saveConfig();
    return;
  }

  if (selectedIndex == 4) {
    ANKI_STORE.cycleFontScale();
    saveConfig();
    return;
  }

  if (selectedIndex == 5) {
    ANKI_STORE.cycleCardOrientation();
    saveConfig();
    return;
  }

  if (selectedIndex == 6) {
    ANKI_STORE.cycleMaxCardsPerDeck();
    saveConfig();
    return;
  }

  if (selectedIndex == 7) {
    ANKI_STORE.cycleMaxCardsTotal();
    saveConfig();
    return;
  }

  if (selectedIndex == 8) {
    ANKI_STORE.cycleCardFontSource();
    saveConfig();
    return;
  }

  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_ANKI_CLEAR_LOCAL),
                                                                tr(STR_ANKI_CLEAR_CONFIRM)),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           std::string error;
                           statusMessage = ANKI_STORE.clearSession(error) ? tr(STR_ANKI_LOCAL_CLEARED) : error;
                           requestUpdate();
                         });
}

void AnkiSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ANKI_SETTINGS),
                 ankiFirmwareVersionLabel());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int statusHeight = statusMessage.empty() ? 0 : renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2 - statusHeight;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, MENU_ITEMS, selectedIndex,
      [](const int index) { return std::string(I18N.get(MENU_NAMES[index])); }, nullptr, nullptr,
      [](const int index) {
        if (index == 0) {
          return ANKI_STORE.getServerUrl().empty() ? std::string(tr(STR_NOT_SET)) : ANKI_STORE.getServerUrl();
        }
        if (index == 1) {
          return std::string("mDNS");
        }
        if (index == 2) {
          return ANKI_STORE.getApiToken().empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        }
        if (index == 3) {
          return std::string(ANKI_STORE.isLeftHanded() ? tr(STR_ANKI_LEFT_HANDED) : tr(STR_ANKI_RIGHT_HANDED));
        }
        if (index == 4) {
          return std::string(fontScaleLabel(ANKI_STORE.getFontScale()));
        }
        if (index == 5) {
          return std::string(ANKI_STORE.isLandscapeCards() ? tr(STR_ANKI_ORIENT_LANDSCAPE)
                                                           : tr(STR_ANKI_ORIENT_PORTRAIT));
        }
        if (index == 6) {
          return formatCount(ANKI_STORE.getMaxCardsPerDeck());
        }
        if (index == 7) {
          return formatCount(ANKI_STORE.getMaxCardsTotal());
        }
        if (index == 8) {
          return std::string(ANKI_STORE.usesReaderFont() ? tr(STR_ANKI_CARD_FONT_READER)
                                                         : tr(STR_ANKI_CARD_FONT_UI));
        }
        return std::string("");
      },
      true);

  if (!statusMessage.empty()) {
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - metrics.buttonHintsHeight - statusHeight,
                              statusMessage.c_str());
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
