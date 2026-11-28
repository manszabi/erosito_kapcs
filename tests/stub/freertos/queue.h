#pragma once
QueueHandle_t xQueueCreate(int,int); int xQueueSend(QueueHandle_t,const void*,int); int xQueueReceive(QueueHandle_t,void*,int);
int xQueueReset(QueueHandle_t);
