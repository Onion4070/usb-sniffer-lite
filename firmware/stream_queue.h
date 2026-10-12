#ifndef STREAM_QUEUE_H
#define STREAM_QUEUE_H

#include <stdint.h>
#include <stdbool.h>

#define STREAM_QUEUE_SIZE 64u
#define STREAM_MAX_WORDS 64u

typedef struct {
    uint32_t timestamp;   // RP2040 timer tick in microseconds (packet end)
    uint32_t bit_count;   // Raw PIO bit count, including PIO terminal convention
    uint32_t word_count;  // Number of valid elements in words[]
    uint32_t words[STREAM_MAX_WORDS];
} stream_packet_t;

// Initialize once before starting Core 1; never reset while active.
void stream_queue_init(void);
// Core 1 only.
bool stream_queue_push(const stream_packet_t *packet);
// Core 0 only.
bool stream_queue_pop(stream_packet_t *packet);
uint32_t stream_queue_dropped(void);
uint32_t stream_queue_pending(void);
uint32_t stream_queue_consumed(void);

#endif