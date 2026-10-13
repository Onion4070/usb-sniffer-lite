#ifndef STREAM_DRAIN_H
#define STREAM_DRAIN_H

#include <stdbool.h>
#include <stdint.h>

uint32_t stream_drain_request(void);
bool stream_drain_complete(uint32_t request);

// Core 0から呼び出す
void stream_drain_task(bool cdc_idle);

#endif