#include "stream_output.h"
#include <string.h>

typedef struct {
    uint16_t length;
    uint8_t bytes[STREAM_OUTPUT_MAX_FRAME];
} output_frame_t;

static output_frame_t queue[STREAM_OUTPUT_SLOTS];
static uint32_t write_index, read_index;
static uint32_t queued, dropped, too_long;

void stream_output_init(void) {
    write_index = read_index = queued = dropped = too_long = 0;
}

bool stream_output_push(uint8_t addr, uint8_t ep, uint8_t pid, uint32_t timestamp,
                        const uint8_t *payload, uint16_t length) {
    if (length > STREAM_OUTPUT_MAX_PAYLOAD) {
        __atomic_store_n(&too_long, too_long + 1u, __ATOMIC_RELAXED);
        return false;
    }
    if ((uint32_t)(write_index - read_index) >= STREAM_OUTPUT_SLOTS) {
        __atomic_store_n(&dropped, dropped + 1u, __ATOMIC_RELAXED);
        return false;
    }
    output_frame_t *frame = &queue[write_index % STREAM_OUTPUT_SLOTS];
    // AA 55 | Address | Endpoint | PID | Timestamp LE32 | Payload Length LE16 | Payload | CRC16-CCITT-FALSE LE16.
    uint8_t *dst = frame->bytes;
    dst[0] = 0xaa;
    dst[1] = 0x55;
    dst[2] = addr;
    dst[3] = ep;
    dst[4] = pid;
    dst[5] = (uint8_t)timestamp;
    dst[6] = (uint8_t)(timestamp >> 8);
    dst[7] = (uint8_t)(timestamp >> 16);
    dst[8] = (uint8_t)(timestamp >> 24);
    dst[9] = (uint8_t)length;
    dst[10] = (uint8_t)(length >> 8);
    memcpy(&dst[11], payload, length);

    // CRC-16/CCITT-FALSE: poly 0x1021, init 0xffff,
    // refin=false, refout=false, xorout=0. Magic excluded.
    uint16_t crc = 0xffffu;
    for (uint16_t i = 2; i < (uint16_t)(11u + length); i++) {
        crc ^= (uint16_t)dst[i] << 8;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    dst[11u + length] = (uint8_t)crc;
    dst[12u + length] = (uint8_t)(crc >> 8);
    frame->length = (uint16_t)(length + 13u);
    write_index++;
    __atomic_store_n(&queued, queued + 1u, __ATOMIC_RELAXED);
    return true;
}

bool stream_output_pop(uint8_t *destination, uint16_t *length) {
    if (write_index == read_index) return false;
    const output_frame_t *frame = &queue[read_index % STREAM_OUTPUT_SLOTS];
    *length = frame->length;
    memcpy(destination, frame->bytes, *length);
    read_index++;
    return true;
}

// Core 0 only: return the number of complete frames that have been queued.
uint32_t stream_output_queued(void) { 
    return __atomic_load_n(&queued, __ATOMIC_RELAXED); 
}

// Core 0 only: return the number of complete frames that have been dropped.
uint32_t stream_output_dropped(void) {
    return __atomic_load_n(&dropped, __ATOMIC_RELAXED);
}

// Core 0 only: return the number of complete frames that have been dropped due to exceeding the maximum payload size.
uint32_t stream_output_too_long(void) { 
    return __atomic_load_n(&too_long, __ATOMIC_RELAXED); 
}

// Core 0 only: return the number of complete frames that have been queued but not yet popped.
uint32_t stream_output_pending(void) { 
    return write_index-read_index; 
}
