#ifndef STREAM_OUTPUT_H
#define STREAM_OUTPUT_H
#include <stdbool.h>
#include <stdint.h>
#define STREAM_OUTPUT_SLOTS 64u
#define STREAM_OUTPUT_MAX_PAYLOAD 192u
#define STREAM_OUTPUT_MAX_FRAME (STREAM_OUTPUT_MAX_PAYLOAD + 8u)
void stream_output_init(void);
// Core 0 only: enqueue a complete, CRC-validated IN response.
bool stream_output_push(uint8_t addr, uint8_t ep, uint8_t pid, uint32_t timestamp,
                        const uint8_t *payload, uint16_t length);
// Core 0 only: read one complete encoded frame.
bool stream_output_pop(uint8_t *destination, uint16_t *length);
uint32_t stream_output_queued(void);
uint32_t stream_output_dropped(void);
uint32_t stream_output_too_long(void);
uint32_t stream_output_pending(void);
#endif
