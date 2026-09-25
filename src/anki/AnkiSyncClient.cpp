#include "AnkiSyncClient.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <algorithm>
#include <array>
#include <cstdio>

#include "AnkiStore.h"

int AnkiSyncClient::lastHttpCode = 0;

namespace {
constexpr int HTTP_BUFFER_SIZE = 2048;
constexpr int HTTP_TIMEOUT_MS = 30000;
constexpr int MAX_PULL_BYTES = 2 * 1024 * 1024;
constexpr size_t MAX_RESPONSE_BYTES = 8192;

bool validBaseUrl(const std::string& value) {
  const bool supported = value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
  return supported && value.find_first_of(" \t\r\n") == std::string::npos;
}

esp_http_client_handle_t createClient(const std::string& url, const esp_http_client_method_t method) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = method;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.buffer_size = HTTP_BUFFER_SIZE;
  config.buffer_size_tx = HTTP_BUFFER_SIZE;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  return esp_http_client_init(&config);
}

bool setCommonHeaders(const esp_http_client_handle_t client, const char* accept) {
  const std::string authorization = "Bearer " + ANKI_STORE.getApiToken();
  return esp_http_client_set_header(client, "Authorization", authorization.c_str()) == ESP_OK &&
         esp_http_client_set_header(client, "Accept", accept) == ESP_OK &&
         esp_http_client_set_header(client, "User-Agent", "CrossInk-Anki/1.6.0") == ESP_OK;
}

void closeClient(const esp_http_client_handle_t client) {
  if (!client) return;
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
}

bool readResponse(const esp_http_client_handle_t client, std::string& response) {
  std::array<char, 512> buffer;
  response.clear();
  while (true) {
    const int read = esp_http_client_read(client, buffer.data(), buffer.size());
    if (read < 0) return false;
    if (read == 0) break;
    if (response.size() + static_cast<size_t>(read) > MAX_RESPONSE_BYTES) return false;
    response.append(buffer.data(), static_cast<size_t>(read));
  }
  return true;
}
}  // namespace

AnkiSyncClient::Error AnkiSyncClient::pull(std::string& detail) {
  lastHttpCode = 0;
  detail.clear();
  if (!ANKI_STORE.configured()) return Error::NOT_CONFIGURED;
  if (ANKI_STORE.hasPendingUpload()) return Error::LOCAL_REVIEWS_PENDING;
  if (!validBaseUrl(ANKI_STORE.getServerUrl())) return Error::INVALID_URL;

  char pullUrl[384];
  snprintf(pullUrl, sizeof(pullUrl), "%s/pull?max_cards=%u&max_total=%u", ANKI_STORE.getServerUrl().c_str(),
           static_cast<unsigned>(ANKI_STORE.getMaxCardsPerDeck()),
           static_cast<unsigned>(ANKI_STORE.getMaxCardsTotal()));
  const std::string url(pullUrl);
  esp_http_client_handle_t client = createClient(url, HTTP_METHOD_GET);
  if (!client || !setCommonHeaders(client, "application/x-ndjson")) {
    closeClient(client);
    return Error::NETWORK_ERROR;
  }

  Storage.remove(AnkiStore::PULL_TEMP_PATH);
  HalFile output;
  if (!Storage.openFileForWrite("ANKI", AnkiStore::PULL_TEMP_PATH, output)) {
    closeClient(client);
    return Error::STORAGE_ERROR;
  }

  esp_err_t result = esp_http_client_open(client, 0);
  if (result != ESP_OK) {
    output.close();
    Storage.remove(AnkiStore::PULL_TEMP_PATH);
    closeClient(client);
    detail = esp_err_to_name(result);
    return Error::NETWORK_ERROR;
  }

  const int64_t contentLength = esp_http_client_fetch_headers(client);
  lastHttpCode = esp_http_client_get_status_code(client);
  if (lastHttpCode == 401 || lastHttpCode == 403) {
    output.close();
    Storage.remove(AnkiStore::PULL_TEMP_PATH);
    closeClient(client);
    return Error::AUTH_FAILED;
  }
  if (lastHttpCode != 200) {
    detail = "HTTP " + std::to_string(lastHttpCode);
    output.close();
    Storage.remove(AnkiStore::PULL_TEMP_PATH);
    closeClient(client);
    return Error::SERVER_ERROR;
  }
  if (contentLength > MAX_PULL_BYTES) {
    output.close();
    Storage.remove(AnkiStore::PULL_TEMP_PATH);
    closeClient(client);
    return Error::RESPONSE_TOO_LARGE;
  }

  std::array<uint8_t, HTTP_BUFFER_SIZE> buffer;
  size_t total = 0;
  while (true) {
    const int read = esp_http_client_read(client, reinterpret_cast<char*>(buffer.data()), buffer.size());
    if (read < 0) {
      output.close();
      Storage.remove(AnkiStore::PULL_TEMP_PATH);
      closeClient(client);
      return Error::NETWORK_ERROR;
    }
    if (read == 0) break;
    total += static_cast<size_t>(read);
    if (total > MAX_PULL_BYTES || output.write(buffer.data(), static_cast<size_t>(read)) != static_cast<size_t>(read)) {
      output.close();
      Storage.remove(AnkiStore::PULL_TEMP_PATH);
      closeClient(client);
      return total > MAX_PULL_BYTES ? Error::RESPONSE_TOO_LARGE : Error::STORAGE_ERROR;
    }
  }

  const bool complete = esp_http_client_is_complete_data_received(client);
  output.flush();
  output.close();
  closeClient(client);
  if (!complete || total == 0) {
    Storage.remove(AnkiStore::PULL_TEMP_PATH);
    return Error::NETWORK_ERROR;
  }

  if (!ANKI_STORE.installPulledBatch(detail)) {
    Storage.remove(AnkiStore::PULL_TEMP_PATH);
    return Error::PROTOCOL_ERROR;
  }
  return Error::OK;
}

AnkiSyncClient::Error AnkiSyncClient::push(std::string& detail) {
  lastHttpCode = 0;
  detail.clear();
  if (!ANKI_STORE.configured()) return Error::NOT_CONFIGURED;
  if (!ANKI_STORE.hasPendingUpload()) return Error::NO_REVIEWS;
  if (!validBaseUrl(ANKI_STORE.getServerUrl())) return Error::INVALID_URL;

  std::string buildError;
  if (!ANKI_STORE.buildPushJson(buildError)) {
    detail = buildError;
    return Error::NO_REVIEWS;
  }

  HalFile body;
  if (!Storage.openFileForRead("ANKI", AnkiStore::PUSH_TEMP_PATH, body)) {
    return Error::STORAGE_ERROR;
  }
  const size_t contentLength = body.size();
  if (contentLength == 0) {
    body.close();
    Storage.remove(AnkiStore::PUSH_TEMP_PATH);
    return Error::NO_REVIEWS;
  }

  const std::string url = ANKI_STORE.getServerUrl() + "/push";
  esp_http_client_handle_t client = createClient(url, HTTP_METHOD_POST);
  if (!client || !setCommonHeaders(client, "application/json") ||
      esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8") != ESP_OK ||
      esp_http_client_set_header(client, "X-Xteink-Batch-ID", ANKI_STORE.getBatchId().c_str()) != ESP_OK) {
    body.close();
    closeClient(client);
    return Error::NETWORK_ERROR;
  }

  esp_err_t result = esp_http_client_open(client, contentLength);
  if (result != ESP_OK) {
    body.close();
    closeClient(client);
    detail = esp_err_to_name(result);
    return Error::NETWORK_ERROR;
  }

  std::array<uint8_t, HTTP_BUFFER_SIZE> buffer;
  while (body.available()) {
    const int read = body.read(buffer.data(), buffer.size());
    if (read <= 0) {
      body.close();
      closeClient(client);
      return Error::STORAGE_ERROR;
    }

    int written = 0;
    while (written < read) {
      const int chunk = esp_http_client_write(client, reinterpret_cast<const char*>(buffer.data()) + written,
                                              static_cast<int>(read) - written);
      if (chunk <= 0) {
        body.close();
        closeClient(client);
        return Error::NETWORK_ERROR;
      }
      written += chunk;
    }
  }
  body.close();

  esp_http_client_fetch_headers(client);
  lastHttpCode = esp_http_client_get_status_code(client);
  std::string response;
  const bool responseRead = readResponse(client, response);
  closeClient(client);
  Storage.remove(AnkiStore::PUSH_TEMP_PATH);

  if (lastHttpCode == 401 || lastHttpCode == 403) return Error::AUTH_FAILED;
  if (lastHttpCode != 200) {
    detail = "HTTP " + std::to_string(lastHttpCode);
    return Error::SERVER_ERROR;
  }
  if (!responseRead) return Error::RESPONSE_TOO_LARGE;

  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, response);
  if (parseError) {
    detail = parseError.c_str();
    return Error::PROTOCOL_ERROR;
  }

  const std::string status = doc["status"] | std::string("");
  if (status == "partial") {
    const int processed = doc["processed"] | 0;
    int rejectedCount = 0;
    if (doc["rejected"].is<JsonArray>()) {
      rejectedCount = static_cast<int>(doc["rejected"].as<JsonArray>().size());
    }
    char summary[96];
    snprintf(summary, sizeof(summary), "%d ok, %d skipped", processed, rejectedCount);
    detail = summary;
    std::string clearError;
    if (!ANKI_STORE.clearSession(clearError)) {
      detail += std::string("; ") + clearError;
      return Error::STORAGE_ERROR;
    }
    return Error::PARTIAL_RESPONSE;
  }
  if (status != "success" && status != "duplicate") {
    detail = response.size() > 200 ? response.substr(0, 200) + "…" : response;
    return Error::PROTOCOL_ERROR;
  }

  if (!ANKI_STORE.clearSession(detail)) return Error::STORAGE_ERROR;
  return Error::OK;
}

const char* AnkiSyncClient::errorString(const Error error) {
  switch (error) {
    case Error::OK:
      return "Success";
    case Error::NOT_CONFIGURED:
      return "Mac server URL or API token is missing";
    case Error::LOCAL_REVIEWS_PENDING:
      return "Upload pending reviews/flags before downloading a new batch";
    case Error::NO_REVIEWS:
      return "No reviews or flags to upload";
    case Error::INVALID_URL:
      return "Server URL must begin with http:// or https://";
    case Error::NETWORK_ERROR:
      // User-facing copy lives in i18n (STR_ANKI_NETWORK_HINT); this is fallback only.
      return "Start Anki on your computer and make sure both devices are on the same Wi-Fi.";
    case Error::AUTH_FAILED:
      return "API token was rejected";
    case Error::SERVER_ERROR:
      return "Mac Anki server returned an error";
    case Error::PROTOCOL_ERROR:
      return "Invalid response from Mac Anki server";
    case Error::STORAGE_ERROR:
      return "SD card write failed";
    case Error::RESPONSE_TOO_LARGE:
      return "Server response is too large";
    case Error::PARTIAL_RESPONSE:
      return "Some reviews were rejected";
  }
  return "Unknown Anki sync error";
}
