#pragma once
#include "Arduino.h"
#include <functional>
typedef enum { HTTP_ANY, HTTP_GET, HTTP_POST } HTTPMethod;
typedef enum { BASIC_AUTH, DIGEST_AUTH } HTTPAuthMethod;
struct WebServer { WebServer(int);
  void on(const char*, HTTPMethod, std::function<void(void)>); void onNotFound(std::function<void(void)>); void begin(); void handleClient();
  bool authenticate(const char*, const char*); void requestAuthentication(HTTPAuthMethod = BASIC_AUTH, const char* = nullptr, const String& = String(""));
  String arg(const char*); void send(int, const char*, const char*); void send(int, const char*, const String&); void sendHeader(const char*, const String&, bool = false);
  void collectHeaders(const char *headerKeys[], const size_t headerKeysCount); String header(const char*); bool hasHeader(const char*); HTTPMethod method();
  void send_P(int, const char*, const char*, size_t); };
