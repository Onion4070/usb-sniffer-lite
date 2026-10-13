#include "stream_drain.h"
#include "stream_queue.h"
#include "stream_output.h"

static uint32_t drain_request;
static uint32_t drain_ack;

uint32_t stream_drain_request(void) {
    uint32_t id = __atomic_load_n(&drain_request, __ATOMIC_RELAXED) + 1u;
    __atomic_store_n(&drain_request, id, __ATOMIC_RELEASE);
    return id;
}

bool stream_drain_complete(uint32_t id) {
    return __atomic_load_n(&drain_ack, __ATOMIC_ACQUIRE) == id;
}

// Core 0: finish all decoded output and CDC transfers BEFORE acknowledging.
void stream_drain_task(bool cdc_idle) {
    uint32_t request = __atomic_load_n(&drain_request, __ATOMIC_ACQUIRE);

    if (request == __atomic_load_n(&drain_ack, __ATOMIC_RELAXED))
        return;

    if (stream_queue_pending() != 0 ||
        stream_output_pending() != 0 ||
        !cdc_idle)
        return;

    __atomic_store_n(&drain_ack, request, __ATOMIC_RELEASE);
}