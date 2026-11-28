#pragma once
#include <stdint.h>
#include <stddef.h>
struct Preferences{bool begin(const char*,bool);void end();bool getBool(const char*,bool);uint8_t getUChar(const char*,uint8_t);size_t putBool(const char*,bool);size_t putUChar(const char*,uint8_t);
 bool isKey(const char*); size_t getString(const char*,char*,size_t); size_t putString(const char*,const char*); bool clear();
 size_t putBytes(const char*,const void*,size_t); size_t getBytes(const char*,void*,size_t); size_t putUInt(const char*,uint32_t); uint32_t getUInt(const char*,uint32_t);};
