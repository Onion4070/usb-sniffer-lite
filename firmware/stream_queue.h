#ifndef STREAM_QUEUE_H
#define STREAM_QUEUE_H

#include <stdint.h>
#include <stdbool.h>

#define STREAM_QUEUE_SIZE 64
#define STREAM_MAX_WORDS 64

typedef struct
{
    uint32_t timestamp;
    uint32_t bit_count;
    uint32_t word_count;
    uint32_t words[STREAM_MAX_WORDS];
} stream_packet_t;

// Core 1: Producer
bool stream_queue_push(const stream_packet_t *packet);

// Core 0: Consumer
bool stream_queue_pop(stream_packet_t *packet);

// 初期化・診断
void stream_queue_init(void);
uint32_t stream_queue_dropped(void);
uint32_t stream_queue_pending(void);

#endif