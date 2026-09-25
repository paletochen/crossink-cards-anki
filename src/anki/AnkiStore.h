#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AnkiCard {
  uint64_t id = 0;
  std::string front;
  std::string back;
  bool isLearning = false;
  // Anki user flag 0–7 (0 = none). X4 toggles 0 ↔ 1 (red flag).
  uint8_t flag = 0;
};

struct AnkiDeckInfo {
  std::string id;
  std::string name;
  uint16_t remaining = 0;
  uint16_t total = 0;
};

class AnkiStore {
 public:
  static constexpr const char* PULL_TEMP_PATH = "/.crosspoint/anki-pull.tmp";
  static constexpr const char* FLAGS_PATH = "/.crosspoint/anki-flags.csv";
  static constexpr const char* PUSH_TEMP_PATH = "/.crosspoint/anki-push.json";

  static AnkiStore& getInstance();

  bool load(std::string& error);
  bool loadConfiguration(std::string& error);
  bool saveConfig(std::string& error) const;
  bool configured() const;

  const std::string& getServerUrl() const { return serverUrl; }
  const std::string& getApiToken() const { return apiToken; }
  const std::string& getBatchId() const { return batchId; }

  void setServerUrl(std::string value);
  void setApiToken(std::string value);
  bool isLeftHanded() const { return leftHanded; }
  void setLeftHanded(bool value);

  // Pull limits sent to the Mac add-on (?max_cards=&max_total=).
  uint16_t getMaxCardsPerDeck() const { return maxCardsPerDeck; }
  uint16_t getMaxCardsTotal() const { return maxCardsTotal; }
  void setMaxCardsPerDeck(uint16_t value);
  void setMaxCardsTotal(uint16_t value);
  void cycleMaxCardsPerDeck();
  void cycleMaxCardsTotal();

  // false (default): UI_12 with built-in DE/Greek glyphs.
  // true: CrossPoint reader font (Noto / SD fonts under Settings → Font / Fonts page).
  bool usesReaderFont() const { return useReaderFont; }
  void setUseReaderFont(bool value);
  void cycleCardFontSource();

  // 1=small, 2=medium (default), 3=large — integer pixel scale for card text
  uint8_t getFontScale() const { return fontScale; }
  void setFontScale(uint8_t scale);
  void cycleFontScale();

  // 0=portrait, 1=landscape
  uint8_t getCardOrientation() const { return cardOrientation; }
  void setCardOrientation(uint8_t orientation);
  void cycleCardOrientation();
  bool isLandscapeCards() const { return cardOrientation != 0; }

  bool installPulledBatch(std::string& error);
  bool loadCurrentCard(AnkiCard& card, std::string& error) const;
  bool recordReview(uint8_t ease, uint32_t durationMs, std::string& error);
  // Toggle Anki red flag (0 ↔ 1) on the current card; persists until push/clear.
  bool toggleCurrentCardFlag(uint8_t& newFlag, std::string& error);
  bool clearSession(std::string& error);

  // Build JSON push body (reviews + dirty flags) to PUSH_TEMP_PATH for streaming upload.
  bool buildPushJson(std::string& error) const;

  bool hasBatch() const { return cardCount > 0; }
  bool hasRemainingCards() const;
  bool hasAnyRemainingCards() const;
  bool hasPendingReviews() const { return reviewCount > 0; }
  bool hasPendingFlags() const;
  bool hasPendingUpload() const { return hasPendingReviews() || hasPendingFlags(); }
  bool batchComplete() const { return hasBatch() && !hasAnyRemainingCards(); }
  uint16_t getCardCount() const { return cardCount; }
  uint16_t getRemainingCount() const;
  uint16_t getAnyRemainingCount() const;
  uint16_t getReviewCount() const { return reviewCount; }
  uint16_t getPendingFlagCount() const;

  uint16_t getDeckCount() const { return static_cast<uint16_t>(decks.size()); }
  uint16_t getCurrentDeckIndex() const { return currentDeck; }
  bool setCurrentDeck(uint16_t deckIndex);
  AnkiDeckInfo getDeckInfo(uint16_t deckIndex) const;
  const std::string& getCurrentDeckName() const;

 private:
  struct DeckSession {
    std::string id;
    std::string name;
    std::vector<uint16_t> queue;
    uint16_t cursor = 0;
    // Progress UI: baseline = cards at pull; completed only when not requeued
    // (Again / Hard-on-learning re-insert and must not fill the progress bar).
    uint16_t baseline = 0;
    uint16_t completed = 0;

    uint16_t remaining() const {
      return cursor < queue.size() ? static_cast<uint16_t>(queue.size() - cursor) : 0;
    }
  };

  AnkiStore() = default;

  static constexpr const char* CONFIG_PATH = "/.crosspoint/anki-config.json";
  static constexpr const char* STATE_PATH = "/.crosspoint/anki-state.json";
  static constexpr const char* CARDS_PATH = "/.crosspoint/anki-cards.ndjson";
  static constexpr const char* INDEX_PATH = "/.crosspoint/anki-index.bin";
  static constexpr const char* INDEX_TEMP_PATH = "/.crosspoint/anki-index.tmp";
  static constexpr const char* REVIEWS_PATH = "/.crosspoint/anki-reviews.csv";
  static constexpr size_t MAX_CARD_LINE_BYTES = 16384;
  static constexpr uint16_t MAX_CARDS = 1000;
  static constexpr uint16_t MAX_DECKS = 64;
  static constexpr size_t MAX_DECK_ID_CHARS = 32;
  static constexpr size_t MAX_DECK_NAME_CHARS = 64;

  bool lookupFlagOverride(uint64_t cardId, uint8_t& flag) const;
  bool writeFlagOverride(uint64_t cardId, uint8_t flag, std::string& error);
  uint8_t clampFlag(int value) const;

  std::string serverUrl;
  std::string apiToken;
  bool leftHanded = false;
  uint16_t maxCardsPerDeck = 250;
  uint16_t maxCardsTotal = 1000;
  bool useReaderFont = false;     // default UI font (Greek/Latin)
  // 1=small (UI_12@1×), 2=medium (UI_18@1× native Greek), 3=large (UI_12@2×).
  uint8_t fontScale = 2;
  uint8_t cardOrientation = 0;    // 0 portrait, 1 landscape
  std::string batchId;
  uint16_t cardCount = 0;
  uint16_t reviewCount = 0;
  uint16_t currentDeck = 0;
  std::vector<DeckSession> decks;
  static const std::string EMPTY_DECK_NAME;

  bool loadSession(std::string& error);
  bool saveSession(std::string& error) const;
  bool loadCardByIndex(uint16_t cardIndex, AnkiCard& card, std::string& error) const;
  void resetSessionState();
  DeckSession* currentDeckSession();
  const DeckSession* currentDeckSession() const;
};


#define ANKI_STORE AnkiStore::getInstance()
