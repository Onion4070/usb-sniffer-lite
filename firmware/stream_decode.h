#ifndef STREAM_DECODE_H
#define STREAM_DECODE_H

#include <stdint.h>
#include "stream_queue.h"

typedef struct {
    uint32_t processed;
    uint32_t valid;
    uint32_t errors;
    uint32_t crc;
    uint32_t pid;
    uint32_t stuff;
    uint32_t sync;
    uint32_t nbit;
    uint32_t size;
    uint32_t in;
    uint32_t data;
    uint32_t sof;
} stream_decode_stats_t;

// Call once on Core 0 before capture_init().
void stream_decode_init(void);
// Core 0 only; consumes one queued raw PIO packet.
void stream_decode_packet(const stream_packet_t *packet);
// Safe to call from Core 1 after capture has stopped.
void stream_decode_get_stats(stream_decode_stats_t *out);

#endif
