#pragma once
#include <stdint.h>
#include "Arduino.h"
typedef enum { WIFI_POWER_15dBm = 60, WIFI_POWER_8_5dBm = 34 } wifi_power_t;
typedef enum { WIFI_OFF, WIFI_STA, WIFI_AP, WIFI_AP_STA } wifi_mode_t;
typedef enum { WL_IDLE_STATUS, WL_DISCONNECTED = 6, WL_CONNECTED = 3 } wl_status_t;
struct WiFiClass { void persistent(bool); bool setHostname(const char*); bool mode(wifi_mode_t); bool setAutoReconnect(bool);
  wl_status_t begin(const char*, const char*); bool setTxPower(wifi_power_t); wl_status_t status(); int8_t RSSI();
  bool disconnect(bool wifioff=false, bool eraseap=false); bool softAP(const char*, const char*); IPAddress softAPIP(); uint8_t softAPgetStationNum();
  bool softAPdisconnect(bool wifioff=false); uint8_t* macAddress(uint8_t*); IPAddress localIP(); bool setSleep(bool); bool config(IPAddress, IPAddress, IPAddress, IPAddress = IPAddress(), IPAddress = IPAddress()); };
extern WiFiClass WiFi;
