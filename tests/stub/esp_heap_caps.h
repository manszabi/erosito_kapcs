#pragma once
#include <stdint.h>
#include <stddef.h>
#define MALLOC_CAP_8BIT 4
#define MALLOC_CAP_INTERNAL 2048
size_t heap_caps_get_largest_free_block(uint32_t);
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_minimum_free_size(uint32_t);
