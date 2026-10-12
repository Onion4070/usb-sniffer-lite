#include "stream_queue.h"

#include <string.h>

static stream_packet_t queue[STREAM_QUEUE_SIZE];

static uint32_t write_index = 0;
static uint32_t read_index = 0;
static uint32_t dropped = 0;

void stream_queue_init(void) {
    __atomic_store_n(&write_index, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&read_index, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&dropped, 0, __ATOMIC_RELAXED);
}

bool stream_queue_push(const stream_packet_t* packet) {
    uint32_t w = __atomic_load_n(&write_index, __ATOMIC_RELAXED);
    uint32_t r = __atomic_load_n(&read_index, __ATOMIC_ACQUIRE);

    if ((uint32_t)(w - r) >= STREAM_QUEUE_SIZE) {
        uint32_t d = __atomic_load_n(&dropped, __ATOMIC_RELAXED);
        __atomic_store_n(&dropped, d + 1, __ATOMIC_RELAXED);
        return false;
    }

    queue[w % STREAM_QUEUE_SIZE] = *packet;
    __atomic_store_n(&write_index, w + 1, __ATOMIC_RELEASE);

    return true;
}

bool stream_queue_pop(stream_packet_t* packet) {
    uint32_t r = __atomic_load_n(&read_index, __ATOMIC_RELAXED);
    uint32_t w = __atomic_load_n(&write_index, __ATOMIC_ACQUIRE);

    if (r == w) return false;

    *packet = queue[r % STREAM_QUEUE_SIZE];
    __atomic_store_n(&read_index, r + 1, __ATOMIC_RELEASE);

    return true;
}

uint32_t stream_queue_dropped(void) {
    return __atomic_load_n(&dropped, __ATOMIC_RELAXED);
}

uint32_t stream_queue_pending(void) {
    uint32_t w = __atomic_load_n(&write_index, __ATOMIC_ACQUIRE);
    uint32_t r = __atomic_load_n(&read_index, __ATOMIC_ACQUIRE);
    return (uint32_t)(w - r);
}

uint32_t stream_queue_consumed(void) {
    return __atomic_load_n(&read_index, __ATOMIC_ACQUIRE);
}
