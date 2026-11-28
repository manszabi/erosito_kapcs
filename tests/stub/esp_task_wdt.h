#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_ERR_INVALID_STATE 3
struct esp_task_wdt_config_t{uint32_t timeout_ms;uint32_t idle_core_mask;bool trigger_panic;};
esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t*);esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t*);esp_err_t esp_task_wdt_add(void*);esp_err_t esp_task_wdt_reset();
