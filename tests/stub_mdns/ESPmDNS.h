#pragma once
#include "Arduino.h"
struct MDNSResponder { bool begin(const char*); void setInstanceName(String); bool addService(const char*, const char*, uint16_t);
  bool addServiceTxt(const char*, const char*, const char*, const char*); };
extern MDNSResponder MDNS;
