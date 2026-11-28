#pragma once
#include <stdio.h>
#include "WString_stub.h"
#include <stdint.h>
#include <stddef.h>
#define ESP_ARDUINO_VERSION_MAJOR 4
#define ESP_ARDUINO_VERSION_MINOR 0
#define ESP_ARDUINO_VERSION_PATCH 0
#if defined(SIM_S3)
#define CONFIG_IDF_TARGET_ESP32S3 1
#define ARDUINO_XIAO_ESP32S3
static const uint8_t D0=1,D1=2,D2=3,D3=4,D4=5,D5=6,D6=43,D7=44,D8=7,D9=8,D10=9;
#else
#define CONFIG_IDF_TARGET_ESP32C3 1
#define ARDUINO_XIAO_ESP32C3
static const uint8_t D0=2,D1=3,D2=4,D3=5,D4=6,D5=7,D6=21,D7=20,D8=8,D9=9,D10=10;
#endif
#define ARDUINO_USB_CDC_ON_BOOT 1
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2
unsigned long millis(); void delay(unsigned long); void digitalWrite(int,int); int digitalRead(int); void pinMode(int,int);
struct S_{void begin(int){} void print(const char*){} int available(){return 0;} int read(){return -1;}}; extern S_ Serial;
