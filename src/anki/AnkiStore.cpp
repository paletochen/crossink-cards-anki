#include "AnkiStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

const std::string AnkiStore::EMPTY_DECK_NAME;

namespace {
bool readLine(HalFile& file, std::string& line, const size_t maxBytes, bool& tooLong) {
  line.clear();
  tooLong = false;
  bool readAnything = false;

  while (file.available()) {
    const int value = file.read();
    if (value < 0) break;
    readAnything = true;
    if (value == '\n') return true;
    if (value == '\r') continue;
    if (line.size() < maxBytes) {
      line.push_back(static_cast<char>(value));
    } else {
      tooLong = true;
    }
  }
  return readAnything;
}

std::string trim(std::string value) {
  const auto first =
      std::find_if_not(value.begin(), value.end(), [](const unsigned char c) { return std::isspace(c); });
  const auto last =
      std::find_if_not(value.rbegin(), value.rend(), [](const unsigned char c) { return std::isspace(c); }).base();
  if (first >= last) return {};
  return std::string(first, last);
}

bool parseCardId(const JsonVariantConst& value, uint64_t& id) {
  if (value.is<const char*>()) {
    const char* text = value.as<const char*>();
    if (!text || !*text) return false;
    char* end = nullptr;
    const unsigned long long parsed = strtoull(text, &end, 10);
    if (!end || *end != '\0' || parsed == 0) return false;
    id = static_cast<uint64_t>(parsed);
    return true;
  }

  if (value.is<uint64_t>()) {
    id = value.as<uint64_t>();
    return id != 0;
  }
  return false;
}

constexpr size_t kMaxDeckIdChars = 32;
constexpr size_t kMaxDeckNameChars = 64;
constexpr uint16_t kMaxDecks = 64;

std::string clipText(std::string value, const size_t maxChars) {
  if (value.size() <= maxChars) return value;
  value.resize(maxChars);
  return value;
}

// Robust JSON text extraction — ArduinoJson string storage can fail is<const char*>()
// for some values while still converting via as<std::string>().
std::string jsonText(const JsonVariantConst& value) {
  if (value.isNull()) return {};
  if (value.is<const char*>()) {
    const char* text = value.as<const char*>();
    return text ? std::string(text) : std::string();
  }
  if (value.is<std::string>()) return value.as<std::string>();
  if (value.is<long long>()) return std::to_string(value.as<long long>());
  if (value.is<unsigned long long>()) return std::to_string(value.as<unsigned long long>());
  if (value.is<int>()) return std::to_string(value.as<int>());
  if (value.is<unsigned int>()) return std::to_string(value.as<unsigned int>());
  return {};
}

bool isPlaceholderDeckName(const std::string& name, const std::string& id) {
  if (name.empty()) return true;
  if (name == "Anki") return true;
  if (!id.empty() && name == id) return true;
  return false;
}

std::string deckKey(const JsonVariantConst& idValue, const JsonVariantConst& nameValue) {
  const std::string idText = clipText(jsonText(idValue), kMaxDeckIdChars);
  if (!idText.empty()) return idText;

  const std::string nameText = clipText(jsonText(nameValue), kMaxDeckNameChars);
  if (!nameText.empty()) return std::string("name:") + nameText;
  return "default";
}

std::string deckDisplayName(const JsonVariantConst& nameValue, const std::string& fallbackId) {
  const std::string text = clipText(jsonText(nameValue), kMaxDeckNameChars);
  if (!text.empty() && !isPlaceholderDeckName(text, fallbackId)) return text;

  if (!fallbackId.empty() && fallbackId != "default") {
    // name:… keys are synthetic ids when the server omitted deck_id.
    if (fallbackId.rfind("name:", 0) == 0) return clipText(fallbackId.substr(5), kMaxDeckNameChars);
    // Prefer a later real name over showing a raw numeric id when we only have a placeholder.
    if (!text.empty()) return text;
    return clipText(fallbackId, kMaxDeckNameChars);
  }
  return text.empty() ? std::string("Anki") : text;
}
}  // namespace

AnkiStore& AnkiStore::getInstance() {
  static AnkiStore instance;
  return instance;
}

bool AnkiStore::load(std::string& error) {
  error.clear();
  if (!loadConfiguration(error)) return false;
  return loadSession(error);
}

bool AnkiStore::loadConfiguration(std::string& error) {
  serverUrl.clear();
  apiToken.clear();
  leftHanded = false;
  maxCardsPerDeck = 250;
  maxCardsTotal = 1000;
  useReaderFont = false;
  fontScale = 2;
  cardOrientation = 0;
  if (!Storage.exists(CONFIG_PATH)) return true;

  const String json = Storage.readFile(CONFIG_PATH);
  if (json.isEmpty()) {
    error = "Could not read Anki configuration";
    return false;
  }

  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, json);
  if (parseError) {
    error = std::string("Invalid Anki configuration: ") + parseError.c_str();
    return false;
  }

  serverUrl = doc["server_url"] | std::string("");
  bool tokenOk = false;
  apiToken = obfuscation::deobfuscateFromBase64(doc["api_token_obf"] | "", &tokenOk);
  if (!tokenOk) apiToken = doc["api_token"] | std::string("");
  leftHanded = doc["left_handed"] | false;
  setMaxCardsPerDeck(static_cast<uint16_t>(doc["max_cards_per_deck"] | 250));
  setMaxCardsTotal(static_cast<uint16_t>(doc["max_cards_total"] | 1000));
  useReaderFont = doc["use_reader_font"] | false;
  const int scale = doc["font_scale"] | 2;
  // scheme 1 (legacy): 1/2/3 = 1×/2×/3×. scheme 2: 1/2/3 = 1×/1.5×/2× (current).
  const int scheme = doc["font_scale_scheme"] | 1;
  if (scheme < 2) {
    // Migrate: keep small; map old medium(2) and large(3) → new large(3)=2×.
    fontScale = scale <= 1 ? 1 : 3;
  } else {
    fontScale = static_cast<uint8_t>(scale < 1 ? 1 : (scale > 3 ? 3 : scale));
  }
  const int orient = doc["card_orientation"] | 0;
  cardOrientation = orient == 0 ? 0 : 1;
  setServerUrl(serverUrl);
  setApiToken(apiToken);
  return true;
}

bool AnkiStore::saveConfig(std::string& error) const {
  JsonDocument doc;
  doc["server_url"] = serverUrl;
  doc["api_token_obf"] = obfuscation::obfuscateToBase64(apiToken);
  doc["left_handed"] = leftHanded;
  doc["max_cards_per_deck"] = maxCardsPerDeck;
  doc["max_cards_total"] = maxCardsTotal;
  doc["use_reader_font"] = useReaderFont;
  doc["font_scale"] = fontScale;
  doc["font_scale_scheme"] = 2;  // 1× / 1.5× / 2×
  doc["card_orientation"] = cardOrientation;

  String json;
  serializeJson(doc, json);
  if (!Storage.writeFile(CONFIG_PATH, json)) {
    error = "Could not save Anki configuration";
    return false;
  }
  error.clear();
  return true;
}

bool AnkiStore::configured() const { return !serverUrl.empty() && !apiToken.empty(); }

void AnkiStore::setServerUrl(std::string value) {
  serverUrl = trim(std::move(value));
  while (serverUrl.size() > 8 && serverUrl.back() == '/') serverUrl.pop_back();
}

void AnkiStore::setApiToken(std::string value) { apiToken = trim(std::move(value)); }

void AnkiStore::setLeftHanded(const bool value) { leftHanded = value; }

void AnkiStore::setMaxCardsPerDeck(const uint16_t value) {
  maxCardsPerDeck = value < 1 ? 1 : (value > MAX_CARDS ? MAX_CARDS : value);
  if (maxCardsPerDeck > maxCardsTotal) maxCardsTotal = maxCardsPerDeck;
}

void AnkiStore::setMaxCardsTotal(const uint16_t value) {
  maxCardsTotal = value < 1 ? 1 : (value > MAX_CARDS ? MAX_CARDS : value);
  if (maxCardsPerDeck > maxCardsTotal) maxCardsPerDeck = maxCardsTotal;
}

namespace {
uint16_t nextLimitStep(const uint16_t current) {
  static constexpr uint16_t kSteps[] = {50, 100, 150, 250, 500, 750, 1000};
  for (const uint16_t step : kSteps) {
    if (step > current) return step;
  }
  return kSteps[0];
}
}  // namespace

void AnkiStore::cycleMaxCardsPerDeck() { setMaxCardsPerDeck(nextLimitStep(maxCardsPerDeck)); }

void AnkiStore::cycleMaxCardsTotal() { setMaxCardsTotal(nextLimitStep(maxCardsTotal)); }

void AnkiStore::setUseReaderFont(const bool value) { useReaderFont = value; }

void AnkiStore::cycleCardFontSource() { useReaderFont = !useReaderFont; }

void AnkiStore::setFontScale(const uint8_t scale) {
  fontScale = scale < 1 ? 1 : (scale > 3 ? 3 : scale);
}

void AnkiStore::cycleFontScale() {
  fontScale = fontScale >= 3 ? 1 : static_cast<uint8_t>(fontScale + 1);
}

void AnkiStore::setCardOrientation(const uint8_t orientation) {
  cardOrientation = orientation == 0 ? 0 : 1;
}

void AnkiStore::cycleCardOrientation() { cardOrientation = cardOrientation == 0 ? 1 : 0; }

void AnkiStore::resetSessionState() {
  batchId.clear();
  cardCount = 0;
  reviewCount = 0;
  currentDeck = 0;
  decks.clear();
}

AnkiStore::DeckSession* AnkiStore::currentDeckSession() {
  if (currentDeck >= decks.size()) return nullptr;
  return &decks[currentDeck];
}

const AnkiStore::DeckSession* AnkiStore::currentDeckSession() const {
  if (currentDeck >= decks.size()) return nullptr;
  return &decks[currentDeck];
}

bool AnkiStore::hasRemainingCards() const {
  const DeckSession* deck = currentDeckSession();
  return deck != nullptr && deck->remaining() > 0;
}

bool AnkiStore::hasAnyRemainingCards() const {
  for (const DeckSession& deck : decks) {
    if (deck.remaining() > 0) return true;
  }
  return false;
}

uint16_t AnkiStore::getRemainingCount() const {
  const DeckSession* deck = currentDeckSession();
  return deck ? deck->remaining() : 0;
}

uint16_t AnkiStore::getAnyRemainingCount() const {
  uint32_t total = 0;
  for (const DeckSession& deck : decks) total += deck.remaining();
  return static_cast<uint16_t>(std::min<uint32_t>(total, 65535));
}

bool AnkiStore::setCurrentDeck(const uint16_t deckIndex) {
  if (deckIndex >= decks.size()) return false;
  if (currentDeck == deckIndex) return true;
  currentDeck = deckIndex;
  std::string error;
  return saveSession(error);
}

AnkiDeckInfo AnkiStore::getDeckInfo(const uint16_t deckIndex) const {
  AnkiDeckInfo info;
  if (deckIndex >= decks.size()) return info;
  const DeckSession& deck = decks[deckIndex];
  info.id = deck.id;
  info.name = deck.name;
  // Progress = completed / baseline. Again and Hard-on-learning requeue and do
  // not increment completed, so they must not advance the bar.
  const uint16_t base =
      deck.baseline > 0 ? deck.baseline : static_cast<uint16_t>(deck.queue.size());
  const uint16_t done = std::min(deck.completed, base);
  info.total = base;
  info.remaining = base > done ? static_cast<uint16_t>(base - done) : 0;
  return info;
}

const std::string& AnkiStore::getCurrentDeckName() const {
  const DeckSession* deck = currentDeckSession();
  return deck ? deck->name : EMPTY_DECK_NAME;
}

bool AnkiStore::loadSession(std::string& error) {
  resetSessionState();
  if (!Storage.exists(STATE_PATH)) return true;

  const String json = Storage.readFile(STATE_PATH);
  if (json.isEmpty()) {
    error = "Could not read local Anki state";
    return false;
  }

  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, json);
  if (parseError) {
    error = std::string("Invalid local Anki state: ") + parseError.c_str();
    return false;
  }

  batchId = doc["batch_id"] | std::string("");
  cardCount = doc["card_count"] | static_cast<uint16_t>(0);
  reviewCount = doc["review_count"] | static_cast<uint16_t>(0);
  currentDeck = doc["current_deck"] | static_cast<uint16_t>(0);

  if (batchId.empty() || cardCount > MAX_CARDS || !Storage.exists(CARDS_PATH) || !Storage.exists(INDEX_PATH)) {
    error = "Local Anki batch is incomplete";
    resetSessionState();
    return false;
  }

  const JsonArrayConst savedDecks = doc["decks"];
  if (!savedDecks.isNull()) {
    if (savedDecks.size() > MAX_DECKS) {
      error = "Local Anki deck list is invalid";
      resetSessionState();
      return false;
    }

    decks.reserve(savedDecks.size());
    for (const JsonVariantConst deckValue : savedDecks) {
      DeckSession deck;
      deck.id = clipText(jsonText(deckValue["id"]), MAX_DECK_ID_CHARS);
      if (deck.id.empty()) deck.id = "default";
      deck.name = clipText(jsonText(deckValue["name"]), MAX_DECK_NAME_CHARS);
      if (deck.name.empty()) {
        deck.name = deck.id.rfind("name:", 0) == 0 ? deck.id.substr(5) : std::string("Anki");
      }
      deck.cursor = deckValue["cursor"] | static_cast<uint16_t>(0);
      deck.baseline = deckValue["baseline"] | static_cast<uint16_t>(0);
      deck.completed = deckValue["completed"] | static_cast<uint16_t>(0);

      const JsonArrayConst savedQueue = deckValue["queue"];
      if (savedQueue.isNull() || savedQueue.size() > std::numeric_limits<uint16_t>::max()) {
        error = "Local Anki queue is invalid";
        resetSessionState();
        return false;
      }
      deck.queue.reserve(savedQueue.size());
      for (const JsonVariantConst value : savedQueue) {
        const uint16_t cardIndex = value.as<uint16_t>();
        if (cardIndex >= cardCount) {
          error = "Local Anki queue contains an invalid card";
          resetSessionState();
          return false;
        }
        deck.queue.push_back(cardIndex);
      }
      if (deck.cursor > deck.queue.size()) {
        error = "Local Anki queue position is invalid";
        resetSessionState();
        return false;
      }
      // Migrate sessions saved before baseline/completed existed.
      if (deck.baseline == 0) {
        deck.baseline = static_cast<uint16_t>(deck.queue.size());
        deck.completed = 0;
      }
      if (deck.completed > deck.baseline) deck.completed = deck.baseline;
      decks.push_back(std::move(deck));
    }
  } else {
    // Legacy single-queue state from older firmware builds.
    DeckSession deck;
    deck.id = "default";
    deck.name = "Anki";
    deck.cursor = doc["cursor"] | static_cast<uint16_t>(0);
    deck.baseline = doc["baseline"] | static_cast<uint16_t>(0);
    deck.completed = doc["completed"] | static_cast<uint16_t>(0);
    const JsonArrayConst savedQueue = doc["queue"];
    if (savedQueue.isNull() || savedQueue.size() > std::numeric_limits<uint16_t>::max()) {
      error = "Local Anki queue is invalid";
      resetSessionState();
      return false;
    }
    deck.queue.reserve(savedQueue.size());
    for (const JsonVariantConst value : savedQueue) {
      const uint16_t cardIndex = value.as<uint16_t>();
      if (cardIndex >= cardCount) {
        error = "Local Anki queue contains an invalid card";
        resetSessionState();
        return false;
      }
      deck.queue.push_back(cardIndex);
    }
    if (deck.cursor > deck.queue.size()) {
      error = "Local Anki queue position is invalid";
      resetSessionState();
      return false;
    }
    if (deck.baseline == 0) {
      deck.baseline = static_cast<uint16_t>(deck.queue.size());
      deck.completed = 0;
    }
    if (deck.completed > deck.baseline) deck.completed = deck.baseline;
    decks.push_back(std::move(deck));
    currentDeck = 0;
  }

  if (decks.empty()) {
    error = "Local Anki batch has no decks";
    resetSessionState();
    return false;
  }
  if (currentDeck >= decks.size()) currentDeck = 0;
  return true;
}

bool AnkiStore::saveSession(std::string& error) const {
  JsonDocument doc;
  doc["batch_id"] = batchId;
  doc["card_count"] = cardCount;
  doc["review_count"] = reviewCount;
  doc["current_deck"] = currentDeck;
  JsonArray savedDecks = doc["decks"].to<JsonArray>();
  for (const DeckSession& deck : decks) {
    JsonObject deckObject = savedDecks.add<JsonObject>();
    deckObject["id"] = deck.id;
    deckObject["name"] = deck.name;
    deckObject["cursor"] = deck.cursor;
    deckObject["baseline"] = deck.baseline;
    deckObject["completed"] = deck.completed;
    JsonArray savedQueue = deckObject["queue"].to<JsonArray>();
    for (const uint16_t cardIndex : deck.queue) savedQueue.add(cardIndex);
  }

  String json;
  serializeJson(doc, json);
  if (!Storage.writeFile(STATE_PATH, json)) {
    error = "Could not save local Anki state";
    return false;
  }
  error.clear();
  return true;
}

bool AnkiStore::installPulledBatch(std::string& error) {
  HalFile cards;
  if (!Storage.openFileForRead("ANKI", PULL_TEMP_PATH, cards)) {
    error = "Downloaded Anki batch is missing";
    return false;
  }

  Storage.remove(INDEX_TEMP_PATH);
  HalFile index;
  if (!Storage.openFileForWrite("ANKI", INDEX_TEMP_PATH, index)) {
    error = "Could not create Anki card index";
    return false;
  }

  std::string line;
  bool tooLong = false;
  if (!readLine(cards, line, MAX_CARD_LINE_BYTES, tooLong) || tooLong) {
    error = "Invalid Anki batch header";
    return false;
  }

  JsonDocument meta;
  DeserializationError parseError = deserializeJson(meta, line);
  const std::string type = meta["type"] | std::string("");
  const std::string status = meta["status"] | std::string("");
  const int protocolVersion = meta["protocol_version"] | 0;
  const uint16_t expectedCount = meta["card_count"] | static_cast<uint16_t>(0);
  const std::string newBatchId = meta["pull_id"] | std::string("");
  // v2 = decks meta; v3 = same pull shape + flag field / JSON push (add-on 2.5+).
  if (parseError || type != "meta" || status != "success" ||
      (protocolVersion != 2 && protocolVersion != 3) || newBatchId.empty() || expectedCount > MAX_CARDS) {
    error = "Unsupported or invalid Anki batch";
    return false;
  }

  std::vector<DeckSession> builtDecks;
  builtDecks.reserve(8);
  auto findOrCreateDeck = [&](const std::string& id, const std::string& name) -> DeckSession* {
    for (DeckSession& deck : builtDecks) {
      if (deck.id != id) continue;
      // First card/meta may only have a placeholder; upgrade when a real name arrives.
      if (isPlaceholderDeckName(deck.name, deck.id) && !isPlaceholderDeckName(name, id)) {
        deck.name = name;
      }
      return &deck;
    }
    if (builtDecks.size() >= kMaxDecks) return nullptr;
    DeckSession deck;
    deck.id = id;
    deck.name = name.empty() ? std::string("Anki") : name;
    builtDecks.push_back(std::move(deck));
    return &builtDecks.back();
  };

  // Prefer the compact meta deck list (id + display name) — always present in protocol v2
  // and much smaller than per-card lines, so names survive even when card JSON is tight on RAM.
  const JsonArrayConst metaDecks = meta["decks"];
  if (!metaDecks.isNull()) {
    for (const JsonVariantConst deckValue : metaDecks) {
      const std::string id = deckKey(deckValue["id"], deckValue["name"]);
      const std::string name = deckDisplayName(deckValue["name"], id);
      if (!findOrCreateDeck(id, name)) {
        error = "Anki batch contains too many decks";
        return false;
      }
    }
  }

  uint16_t actualCount = 0;
  bool sawEnd = false;
  while (cards.available()) {
    const size_t offsetValue = cards.position();
    if (offsetValue > std::numeric_limits<uint32_t>::max()) {
      error = "Anki card file is too large";
      return false;
    }
    if (!readLine(cards, line, MAX_CARD_LINE_BYTES, tooLong)) break;
    if (tooLong) {
      error = "Anki card exceeds the device limit";
      return false;
    }
    if (line.empty()) continue;

    JsonDocument doc;
    parseError = deserializeJson(doc, line);
    if (parseError) {
      error = "Anki batch contains invalid JSON";
      return false;
    }

    const std::string recordType = doc["type"] | std::string("");
    if (recordType == "end") {
      const uint16_t endCount = doc["card_count"] | static_cast<uint16_t>(0);
      sawEnd = endCount == actualCount;
      break;
    }
    if (recordType != "card" || actualCount >= MAX_CARDS) {
      error = "Anki batch contains an invalid record";
      return false;
    }

    uint64_t id = 0;
    if (!parseCardId(doc["id"], id)) {
      error = "Anki batch contains an invalid card";
      return false;
    }
    // front/back may be empty strings (media-only or odd templates); UI shows a placeholder.

    const std::string cardDeckId = deckKey(doc["deck_id"], doc["deck_name"]);
    const std::string cardDeckName = deckDisplayName(doc["deck_name"], cardDeckId);
    DeckSession* deck = findOrCreateDeck(cardDeckId, cardDeckName);
    if (!deck) {
      error = "Anki batch contains too many decks";
      return false;
    }
    deck->queue.push_back(actualCount);

    const uint32_t offset = static_cast<uint32_t>(offsetValue);
    if (index.write(&offset, sizeof(offset)) != sizeof(offset)) {
      error = "Could not write Anki card index";
      return false;
    }
    actualCount++;
  }

  cards.close();
  index.flush();
  index.close();
  if (!sawEnd || actualCount != expectedCount) {
    error = "Anki batch ended unexpectedly";
    return false;
  }
  if (builtDecks.empty() && actualCount > 0) {
    error = "Anki batch has cards without decks";
    return false;
  }
  if (actualCount == 0) {
    // Empty successful pull still clears any previous local session.
    Storage.remove(CARDS_PATH);
    Storage.remove(INDEX_PATH);
    Storage.remove(REVIEWS_PATH);
    Storage.remove(FLAGS_PATH);
    Storage.remove(STATE_PATH);
    Storage.remove(PULL_TEMP_PATH);
    Storage.remove(INDEX_TEMP_PATH);
    Storage.remove(PUSH_TEMP_PATH);
    resetSessionState();
    batchId = newBatchId;
    error.clear();
    return true;
  }

  Storage.remove(CARDS_PATH);
  Storage.remove(INDEX_PATH);
  Storage.remove(REVIEWS_PATH);
  Storage.remove(FLAGS_PATH);
  Storage.remove(STATE_PATH);
  Storage.remove(PUSH_TEMP_PATH);
  if (!Storage.rename(PULL_TEMP_PATH, CARDS_PATH) || !Storage.rename(INDEX_TEMP_PATH, INDEX_PATH)) {
    error = "Could not install downloaded Anki batch";
    return false;
  }

  batchId = newBatchId;
  cardCount = actualCount;
  reviewCount = 0;
  currentDeck = 0;
  decks = std::move(builtDecks);
  for (DeckSession& deck : decks) {
    deck.cursor = 0;
    deck.baseline = static_cast<uint16_t>(deck.queue.size());
    deck.completed = 0;
  }

  // Prefer the first deck that still has cards.
  for (uint16_t i = 0; i < decks.size(); i++) {
    if (decks[i].remaining() > 0) {
      currentDeck = i;
      break;
    }
  }
  return saveSession(error);
}

bool AnkiStore::loadCardByIndex(const uint16_t cardIndex, AnkiCard& card, std::string& error) const {
  if (cardIndex >= cardCount) {
    error = "Anki card index is out of range";
    return false;
  }

  HalFile index;
  if (!Storage.openFileForRead("ANKI", INDEX_PATH, index) ||
      !index.seek(static_cast<size_t>(cardIndex) * sizeof(uint32_t))) {
    error = "Could not read Anki card index";
    return false;
  }

  uint32_t offset = 0;
  if (index.read(&offset, sizeof(offset)) != sizeof(offset)) {
    error = "Anki card index is truncated";
    return false;
  }
  index.close();

  HalFile cards;
  if (!Storage.openFileForRead("ANKI", CARDS_PATH, cards) || !cards.seek(offset)) {
    error = "Could not open local Anki cards";
    return false;
  }

  std::string line;
  bool tooLong = false;
  if (!readLine(cards, line, MAX_CARD_LINE_BYTES, tooLong) || tooLong) {
    error = "Local Anki card is invalid";
    return false;
  }

  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, line);
  uint64_t id = 0;
  if (parseError || (doc["type"] | std::string("")) != "card" || !parseCardId(doc["id"], id)) {
    error = "Local Anki card contains invalid data";
    return false;
  }

  card.id = id;
  card.front = doc["front"].is<const char*>() ? doc["front"].as<std::string>() : std::string("");
  card.back = doc["back"].is<const char*>() ? doc["back"].as<std::string>() : std::string("");
  // Keep whitespace-only as empty so the UI can show a clear placeholder.
  auto stripSides = [](std::string& value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\n' || value.back() == '\r')) {
      value.pop_back();
    }
    size_t start = 0;
    while (start < value.size() &&
           (value[start] == ' ' || value[start] == '\t' || value[start] == '\n' || value[start] == '\r')) {
      start++;
    }
    if (start > 0) value.erase(0, start);
  };
  stripSides(card.front);
  stripSides(card.back);
  card.isLearning = doc["is_learning"] | false;
  card.flag = clampFlag(doc["flag"] | 0);
  uint8_t overrideFlag = 0;
  if (lookupFlagOverride(card.id, overrideFlag)) {
    card.flag = overrideFlag;
  }
  error.clear();
  return true;
}

bool AnkiStore::loadCurrentCard(AnkiCard& card, std::string& error) const {
  if (!hasRemainingCards()) {
    error = "No Anki cards remain";
    return false;
  }
  const DeckSession* deck = currentDeckSession();
  return loadCardByIndex(deck->queue[deck->cursor], card, error);
}

uint8_t AnkiStore::clampFlag(const int value) const {
  if (value < 0) return 0;
  if (value > 7) return 7;
  return static_cast<uint8_t>(value);
}

bool AnkiStore::lookupFlagOverride(const uint64_t cardId, uint8_t& flag) const {
  if (!Storage.exists(FLAGS_PATH)) return false;
  HalFile file;
  if (!Storage.openFileForRead("ANKI", FLAGS_PATH, file)) return false;
  std::string line;
  bool tooLong = false;
  bool found = false;
  while (readLine(file, line, 96, tooLong)) {
    if (tooLong || line.empty()) continue;
    // card_id,flag
    const size_t comma = line.find(',');
    if (comma == std::string::npos) continue;
    char* end = nullptr;
    const unsigned long long id = strtoull(line.c_str(), &end, 10);
    if (!end || end == line.c_str() || static_cast<size_t>(end - line.c_str()) != comma) continue;
    if (static_cast<uint64_t>(id) != cardId) continue;
    const int parsed = atoi(line.c_str() + comma + 1);
    flag = clampFlag(parsed);
    found = true;
    // keep scanning so the last write wins
  }
  file.close();
  return found;
}

bool AnkiStore::writeFlagOverride(const uint64_t cardId, const uint8_t flag, std::string& error) {
  // Rewrite FLAGS_PATH with this card updated (last entry wins on read; rewrite keeps file small).
  struct Entry {
    uint64_t id;
    uint8_t flag;
  };
  Entry entries[64];
  size_t count = 0;
  bool replaced = false;

  if (Storage.exists(FLAGS_PATH)) {
    HalFile in;
    if (Storage.openFileForRead("ANKI", FLAGS_PATH, in)) {
      std::string line;
      bool tooLong = false;
      while (readLine(in, line, 96, tooLong)) {
        if (tooLong || line.empty()) continue;
        const size_t comma = line.find(',');
        if (comma == std::string::npos) continue;
        char* end = nullptr;
        const unsigned long long id = strtoull(line.c_str(), &end, 10);
        if (!end || end == line.c_str()) continue;
        const int parsed = atoi(line.c_str() + comma + 1);
        const uint8_t f = clampFlag(parsed);
        const uint64_t uid = static_cast<uint64_t>(id);
        if (uid == cardId) {
          if (count < 64) {
            entries[count++] = Entry{uid, clampFlag(flag)};
            replaced = true;
          }
          continue;
        }
        if (count < 64) entries[count++] = Entry{uid, f};
      }
      in.close();
    }
  }
  if (!replaced && count < 64) {
    entries[count++] = Entry{cardId, clampFlag(flag)};
  }

  Storage.remove(FLAGS_PATH);
  if (count == 0) {
    error.clear();
    return true;
  }
  HalFile out = Storage.open(FLAGS_PATH, O_WRONLY | O_CREAT | O_TRUNC);
  if (!out) {
    error = "Could not write Anki flag overrides";
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    char row[48];
    const int length =
        snprintf(row, sizeof(row), "%llu,%u\n", static_cast<unsigned long long>(entries[i].id),
                 static_cast<unsigned int>(entries[i].flag));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(row) ||
        out.write(row, static_cast<size_t>(length)) != static_cast<size_t>(length)) {
      out.close();
      error = "Could not write Anki flag overrides";
      return false;
    }
  }
  out.flush();
  out.close();
  error.clear();
  return true;
}

bool AnkiStore::hasPendingFlags() const {
  if (!Storage.exists(FLAGS_PATH)) return false;
  HalFile file;
  if (!Storage.openFileForRead("ANKI", FLAGS_PATH, file)) return false;
  const size_t size = file.size();
  file.close();
  return size > 0;
}

uint16_t AnkiStore::getPendingFlagCount() const {
  if (!Storage.exists(FLAGS_PATH)) return 0;
  HalFile file;
  if (!Storage.openFileForRead("ANKI", FLAGS_PATH, file)) return 0;
  std::string line;
  bool tooLong = false;
  uint16_t count = 0;
  while (readLine(file, line, 96, tooLong)) {
    if (!tooLong && !line.empty() && line.find(',') != std::string::npos) count++;
  }
  file.close();
  return count;
}

bool AnkiStore::toggleCurrentCardFlag(uint8_t& newFlag, std::string& error) {
  AnkiCard card;
  if (!loadCurrentCard(card, error)) return false;
  // X4 UI toggles red flag only (0 ↔ 1). Other Anki colours (2–7) from pull clear to 0 first.
  newFlag = (card.flag == 0) ? 1 : 0;
  if (!writeFlagOverride(card.id, newFlag, error)) return false;
  return true;
}

bool AnkiStore::buildPushJson(std::string& error) const {
  if (!hasPendingUpload()) {
    error = "Nothing to upload";
    return false;
  }

  Storage.remove(PUSH_TEMP_PATH);
  HalFile out = Storage.open(PUSH_TEMP_PATH, O_WRONLY | O_CREAT | O_TRUNC);
  if (!out) {
    error = "Could not create Anki push body";
    return false;
  }

  auto writeStr = [&](const char* s) -> bool {
    const size_t n = strlen(s);
    return out.write(s, n) == n;
  };

  if (!writeStr("{\"batch_id\":\"")) {
    out.close();
    error = "Could not write Anki push body";
    return false;
  }
  // batch_id is printable ASCII (protocol); escape is unnecessary.
  if (!writeStr(batchId.c_str()) || !writeStr("\",\"reviews\":[")) {
    out.close();
    error = "Could not write Anki push body";
    return false;
  }

  bool firstReview = true;
  if (Storage.exists(REVIEWS_PATH)) {
    HalFile reviews;
    if (Storage.openFileForRead("ANKI", REVIEWS_PATH, reviews)) {
      std::string line;
      bool tooLong = false;
      while (readLine(reviews, line, 128, tooLong)) {
        if (tooLong || line.empty()) continue;
        // card_id,ease,,duration_ms
        unsigned long long cardId = 0;
        unsigned ease = 0;
        unsigned long duration = 0;
        if (sscanf(line.c_str(), "%llu,%u,,%lu", &cardId, &ease, &duration) < 2) {
          // try with optional answered_at field empty variants
          if (sscanf(line.c_str(), "%llu,%u", &cardId, &ease) < 2) continue;
        }
        if (ease < 1 || ease > 4 || cardId == 0) continue;
        char item[96];
        const int n = snprintf(item, sizeof(item), "%s{\"card_id\":%llu,\"ease\":%u,\"duration_ms\":%lu}",
                               firstReview ? "" : ",", cardId, ease, duration);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(item) ||
            out.write(item, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
          reviews.close();
          out.close();
          error = "Could not write Anki push reviews";
          return false;
        }
        firstReview = false;
      }
      reviews.close();
    }
  }

  if (!writeStr("],\"flags\":[")) {
    out.close();
    error = "Could not write Anki push body";
    return false;
  }

  bool firstFlag = true;
  if (Storage.exists(FLAGS_PATH)) {
    HalFile flags;
    if (Storage.openFileForRead("ANKI", FLAGS_PATH, flags)) {
      std::string line;
      bool tooLong = false;
      while (readLine(flags, line, 96, tooLong)) {
        if (tooLong || line.empty()) continue;
        unsigned long long cardId = 0;
        unsigned flag = 0;
        if (sscanf(line.c_str(), "%llu,%u", &cardId, &flag) < 2 || cardId == 0) continue;
        if (flag > 7) flag = 7;
        char item[72];
        const int n = snprintf(item, sizeof(item), "%s{\"card_id\":%llu,\"flag\":%u}", firstFlag ? "" : ",",
                               cardId, flag);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(item) ||
            out.write(item, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
          flags.close();
          out.close();
          error = "Could not write Anki push flags";
          return false;
        }
        firstFlag = false;
      }
      flags.close();
    }
  }

  if (!writeStr("]}")) {
    out.close();
    error = "Could not write Anki push body";
    return false;
  }
  out.flush();
  out.close();
  if (firstReview && firstFlag) {
    Storage.remove(PUSH_TEMP_PATH);
    error = "Nothing to upload";
    return false;
  }
  error.clear();
  return true;
}

bool AnkiStore::recordReview(const uint8_t ease, const uint32_t durationMs, std::string& error) {
  if (ease < 1 || ease > 4 || !hasRemainingCards()) {
    error = "Invalid Anki review";
    return false;
  }

  DeckSession* deck = currentDeckSession();
  if (!deck) {
    error = "Invalid Anki deck";
    return false;
  }

  AnkiCard card;
  if (!loadCurrentCard(card, error)) return false;

  HalFile reviews = Storage.open(REVIEWS_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!reviews) {
    error = "Could not open the local Anki review log";
    return false;
  }

  char row[96];
  const int length = snprintf(row, sizeof(row), "%llu,%u,,%lu\n", static_cast<unsigned long long>(card.id),
                              static_cast<unsigned int>(ease), static_cast<unsigned long>(durationMs));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(row) ||
      reviews.write(row, static_cast<size_t>(length)) != static_cast<size_t>(length)) {
    error = "Could not write the local Anki review log";
    return false;
  }
  reviews.flush();
  reviews.close();

  const uint16_t reviewedCardIndex = deck->queue[deck->cursor];
  deck->cursor++;
  reviewCount++;

  // Again always, and Hard on learning steps: re-insert into the queue. These
  // do not count toward progress (baseline/completed).
  const bool requeue = (ease == 1) || (ease == 2 && card.isLearning);
  if (requeue) {
    const size_t gap = ease == 1 ? 5 : 10;
    const size_t insertAt = std::min(deck->queue.size(), static_cast<size_t>(deck->cursor) + gap);
    deck->queue.insert(deck->queue.begin() + insertAt, reviewedCardIndex);
  } else {
    if (deck->baseline == 0) deck->baseline = static_cast<uint16_t>(deck->queue.size());
    if (deck->completed < deck->baseline) deck->completed++;
  }

  return saveSession(error);
}

bool AnkiStore::clearSession(std::string& error) {
  bool ok = true;
  for (const char* path : {STATE_PATH, CARDS_PATH, INDEX_PATH, REVIEWS_PATH, FLAGS_PATH, PUSH_TEMP_PATH,
                           PULL_TEMP_PATH, INDEX_TEMP_PATH}) {
    if (Storage.exists(path) && !Storage.remove(path)) ok = false;
  }
  resetSessionState();
  if (!ok) {
    error = "Could not remove all local Anki data";
    return false;
  }
  error.clear();
  return true;
}
