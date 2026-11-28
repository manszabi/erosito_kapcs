#!/bin/sh
# a program másolata 32 bites időtípusokkal (mint az ESP32-C3-on)
sed -e 's/unsigned long/uint32_t/g' -e 's/(long)/(int32_t)/g' -e 's/%lu/%u/g' "$1" > prog.inc
