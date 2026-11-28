#pragma once
int xTaskCreate(void(*)(void*),const char*,int,void*,int,void*);
void vTaskDelay(unsigned);
int xTaskCreatePinnedToCore(void(*)(void*),const char*,int,void*,UBaseType_t,void*,BaseType_t);
typedef void* TaskHandle_t; TaskHandle_t xTaskGetCurrentTaskHandle(); UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t);
