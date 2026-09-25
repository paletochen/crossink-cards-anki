#pragma once

#include <string>

class AnkiSyncClient {
 public:
  enum class Error {
    OK = 0,
    NOT_CONFIGURED,
    LOCAL_REVIEWS_PENDING,
    NO_REVIEWS,
    INVALID_URL,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    PROTOCOL_ERROR,
    STORAGE_ERROR,
    RESPONSE_TOO_LARGE,
    PARTIAL_RESPONSE,
  };

  static Error pull(std::string& detail);
  static Error push(std::string& detail);
  static const char* errorString(Error error);
  static int lastHttpCode;
};
