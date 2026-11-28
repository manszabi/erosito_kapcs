#pragma once
#include "Arduino.h"
struct DNSServer { bool start(uint16_t, const char*, const IPAddress&); void stop(); void processNextRequest(); };
