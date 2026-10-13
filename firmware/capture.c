// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2022, Alex Taradov <alex@taradov.com>. All rights reserved.

/*- Includes ----------------------------------------------------------------*/
#include "capture.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "globals.h"
#include "stream_queue.h"
#include "stream_decode.h"
#include "stream_output.h"
#include "stream_drain.h"
#include "hal_gpio.h"
#include "pio_asm.h"
#include "rp2040.h"

/*- Definitions -------------------------------------------------------------*/
#define CORE1_STACK_SIZE 512  // words

// DP and DM can be any pins, but they must be consequitive and in that order
#define DP_INDEX 10
#define DM_INDEX 11
#define START_INDEX 12

HAL_GPIO_PIN(DP, 0, 10, pio0_10)
HAL_GPIO_PIN(DM, 0, 11, pio0_11)
HAL_GPIO_PIN(START, 0, 12, pio1_12)  // Internal trigger from PIO1 to PIO0
HAL_GPIO_PIN(TRIGGER, 0, 18, sio_18)

/*- Constants ---------------------------------------------------------------*/

static const char* capture_speed_str[CaptureSpeedCount] = {
    [CaptureSpeed_Low] = "Low",
    [CaptureSpeed_Full] = "Full",
};

static const char* capture_trigger_str[CaptureTriggerCount] = {
    [CaptureTrigger_Enabled] = "Enabled",
    [CaptureTrigger_Disabled] = "Disabled",
};

static const char* capture_limit_str[CaptureLimitCount] = {
    [CaptureLimit_100] = "100 packets",
    [CaptureLimit_200] = "200 packets",
    [CaptureLimit_500] = "500 packets",
    [CaptureLimit_1000] = "1000 packets",
    [CaptureLimit_2000] = "2000 packets",
    [CaptureLimit_5000] = "5000 packets",
    [CaptureLimit_10000] = "10000 packets",
    [CaptureLimit_Unlimited] = "Unlimited",
};

static const char* display_time_str[DisplayTimeCount] = {
    [DisplayTime_First] = "Relative to the first packet",
    [DisplayTime_Previous] = "Relative to the previous packet",
    [DisplayTime_SOF] = "Relative to the SOF",
    [DisplayTime_Reset] = "Relative to the bus reset",
};

static const char* display_data_str[DisplayDataCount] = {
    [DisplayData_Full] = "Full",
    [DisplayData_Limit16] = "Limit to 16 bytes",
    [DisplayData_Limit64] = "Limit to 64 bytes",
    [DisplayData_None] = "Do not display data",
};

static const char* display_fold_str[DisplayFoldCount] = {
    [DisplayFold_Enabled] = "Enabled",
    [DisplayFold_Disabled] = "Disabled",
};

/*- Variables ---------------------------------------------------------------*/
uint32_t g_buffer[BUFFER_SIZE];
buffer_info_t g_buffer_info;

int g_capture_speed = CaptureSpeed_Full;
int g_capture_trigger = CaptureTrigger_Disabled;
int g_capture_limit = CaptureLimit_Unlimited;
int g_display_time = DisplayTime_SOF;
int g_display_data = DisplayData_Full;
int g_display_fold = DisplayFold_Enabled;

/*- Implementations ---------------------------------------------------------*/

//-----------------------------------------------------------------------------
static int capture_limit_value(void) {
    if (g_capture_limit == CaptureLimit_100)
        return 100;
    else if (g_capture_limit == CaptureLimit_200)
        return 200;
    else if (g_capture_limit == CaptureLimit_500)
        return 500;
    else if (g_capture_limit == CaptureLimit_1000)
        return 1000;
    else if (g_capture_limit == CaptureLimit_2000)
        return 2000;
    else if (g_capture_limit == CaptureLimit_5000)
        return 5000;
    else if (g_capture_limit == CaptureLimit_10000)
        return 10000;
    else
        return 0;  // Unlimited
}

//-----------------------------------------------------------------------------
static int poll_cmd(void) {
    if (SIO->FIFO_ST & SIO_FIFO_ST_VLD_Msk) return SIO->FIFO_RD;
    return 0;
}

//-----------------------------------------------------------------------------
static bool wait_for_trigger(void) {
    if (!g_buffer_info.trigger) return true;

    display_puts("Waiting for a trigger\r\n");

    while (1) {
        if (poll_cmd() == 'p') return false;
        if (HAL_GPIO_TRIGGER_read() == 0) return true;
    }
}

//-----------------------------------------------------------------------------
static void capture_buffer(void) {
    volatile uint32_t* PIO0_INSTR_MEM = (volatile uint32_t*)&PIO0->INSTR_MEM0;
    volatile uint32_t* PIO1_INSTR_MEM = (volatile uint32_t*)&PIO1->INSTR_MEM0;

    HAL_GPIO_DP_init();
    HAL_GPIO_DM_init();
    HAL_GPIO_START_init();

    RESETS_SET->RESET = RESETS_RESET_pio0_Msk | RESETS_RESET_pio1_Msk;
    RESETS_CLR->RESET = RESETS_RESET_pio0_Msk | RESETS_RESET_pio1_Msk;
    while (0 == RESETS->RESET_DONE_b.pio0 && 0 == RESETS->RESET_DONE_b.pio1);

    g_buffer_info.fs = (g_capture_speed == CaptureSpeed_Full);
    g_buffer_info.trigger = (g_capture_trigger == CaptureTrigger_Enabled);
    g_buffer_info.limit = capture_limit_value();

    static const uint16_t pio0_ops[] = {
        // idle:
        /* 0 */ OP_MOV | MOV_DST_X | MOV_SRC_NULL |
            MOV_OP_INVERT,  // Reset the bit counter
        /* 1 */ OP_WAIT | WAIT_POL_1 | WAIT_SRC_PIN |
            WAIT_INDEX(0),  // Wait until the bus goes idle
        /* 2 */ OP_WAIT | WAIT_POL_0 | WAIT_SRC_PIN |
            WAIT_INDEX(0),  // Wait for the SOP

        // start0:
        /* 3 */ OP_NOP | OP_DELAY(1),  // Skip to the middle of the bit

        // read0:
        /* 4 */ OP_JMP | JMP_COND_X_NZ_PD |
            JMP_ADDR(5 /*next*/),                 // Decrement the bit counter
        /* 5 */ OP_IN | IN_SRC_PINS | IN_CNT(1),  // Sample D+
        /* 6 */ OP_MOV | MOV_DST_OSR | MOV_SRC_PINS |
            MOV_OP_BIT_REV,  // Sample D+ and D-
        /* 7 */ OP_OUT | OUT_DST_Y | OUT_CNT(2),
        /* 8 */ OP_JMP | JMP_COND_Y_ZERO |
            JMP_ADDR(21 /*eop*/),      // If both are 0, then it is an EOP
        /* 9 */ OP_NOP | OP_DELAY(3),  // Skip to the middle of the bit
        /* 10 */ OP_JMP | JMP_COND_PIN |
            JMP_ADDR(4 /*read0*/),  // If D- is high, then D+ is low, read 0

        // read1:
        /* 11 */ OP_JMP | JMP_COND_X_NZ_PD |
            JMP_ADDR(12 /*next*/),                 // Decrement the bit counter
        /* 12 */ OP_IN | IN_SRC_PINS | IN_CNT(1),  // Sample D+
        /* 13 */ OP_MOV | MOV_DST_OSR | MOV_SRC_PINS |
            MOV_OP_BIT_REV,  // Sample D+ and D-
        /* 14 */ OP_OUT | OUT_DST_Y | OUT_CNT(2),
        /* 15 */ OP_JMP | JMP_COND_Y_ZERO |
            JMP_ADDR(21 /*eop*/),  // If both are 0, then it is an EOP
        /* 16 */ OP_JMP | JMP_COND_PIN |
            JMP_ADDR(3 /*start0*/),  // Look for a low to high transition on
        /* 17 */ OP_JMP | JMP_COND_PIN |
            JMP_ADDR(3 /*start0*/),  // D- to adjust the sample point location
        /* 18 */ OP_JMP | JMP_COND_PIN | JMP_ADDR(3 /*start0*/),
        /* 19 */ OP_JMP | JMP_COND_PIN | JMP_ADDR(3 /*start0*/),
        /* 20 */ OP_JMP | JMP_ADDR(11 /*read1*/),

        // eop:
        /* 21 */ OP_PUSH,                           // Transfer the last data
        /* 22 */ OP_MOV | MOV_DST_ISR | MOV_SRC_X,  // Transfer the bit count
        /* 23 */ OP_PUSH,

        // poll_reset:
        /* 24 */ OP_SET | SET_DST_X | SET_DATA(31),

        // poll_loop:
        /* 25 */ OP_MOV | MOV_DST_OSR | MOV_SRC_PINS |
            MOV_OP_BIT_REV,  // Sample D+ and D-
        /* 26 */ OP_OUT | OUT_DST_Y | OUT_CNT(2),
        /* 27 */ OP_JMP | JMP_COND_Y_NZ_PD |
            JMP_ADDR(0 /*idle*/),  // If either is not zero, back to idle
        /* 28 */ OP_JMP | JMP_COND_X_NZ_PD | JMP_ADDR(25 /*poll_loop*/),
        /* 29 */ OP_MOV | MOV_DST_ISR | MOV_SRC_NULL | MOV_OP_INVERT,
        /* 30 */ OP_PUSH,
        // Wrap to 0 from here

        // Entry point, wait for a START signal from the PIO1
        /* 31 */ OP_WAIT | WAIT_POL_1 | WAIT_SRC_PIN | WAIT_INDEX(2),
    };

    static const uint16_t pio1_ops[] = {
        /* 0 */ OP_NOP | OP_DELAY(31),  // Wait for the PIO0 to start
        /* 1 */ OP_NOP | OP_DELAY(31),
        /* 2 */ OP_NOP | OP_DELAY(31),
        /* 3 */ OP_NOP | OP_DELAY(31),

        // wait_se0:
        /* 4 */ OP_MOV | MOV_DST_OSR | MOV_SRC_PINS | MOV_OP_BIT_REV,
        /* 5 */ OP_OUT | OUT_DST_Y | OUT_CNT(2),
        /* 6 */ OP_JMP | JMP_COND_Y_NZ_PD | JMP_ADDR(4 /*wait_se0*/),

        /* 7 */ OP_MOV | MOV_DST_OSR | MOV_SRC_PINS | MOV_OP_BIT_REV,
        /* 8 */ OP_OUT | OUT_DST_Y | OUT_CNT(2),
        /* 9 */ OP_JMP | JMP_COND_Y_NZ_PD | JMP_ADDR(4 /*wait_se0*/),

        /* 10 */ OP_MOV | MOV_DST_OSR | MOV_SRC_PINS | MOV_OP_BIT_REV,
        /* 11 */ OP_OUT | OUT_DST_Y | OUT_CNT(2),
        /* 12 */ OP_JMP | JMP_COND_Y_NZ_PD | JMP_ADDR(4 /*wait_se0*/),

        /* 13 */ OP_MOV | MOV_DST_OSR | MOV_SRC_PINS | MOV_OP_BIT_REV,
        /* 14 */ OP_OUT | OUT_DST_Y | OUT_CNT(2),
        /* 15 */ OP_JMP | JMP_COND_Y_NZ_PD | JMP_ADDR(4 /*wait_se0*/),

        /* 16 */ OP_SET | SET_DST_PINS | SET_DATA(1),  // Set the START output
        /* 17 */ OP_JMP | JMP_ADDR(17 /*self*/),       // Infinite loop
    };

    // PIO0 init
    PIO0->SM0_CLKDIV = ((g_buffer_info.fs ? 1 : 8) << PIO0_SM0_CLKDIV_INT_Pos);

    for (int i = 0; i < (int)(sizeof(pio0_ops) / sizeof(uint16_t)); i++)
        PIO0_INSTR_MEM[i] = pio0_ops[i];

    if (!g_buffer_info.fs) {
        PIO0_INSTR_MEM[1] = OP_WAIT | WAIT_POL_1 | WAIT_SRC_PIN | WAIT_INDEX(1);
        PIO0_INSTR_MEM[2] = OP_WAIT | WAIT_POL_0 | WAIT_SRC_PIN | WAIT_INDEX(1);
    }

    PIO0->SM0_EXECCTRL = ((g_buffer_info.fs ? DM_INDEX : DP_INDEX)
                          << PIO0_SM0_EXECCTRL_JMP_PIN_Pos) |
                         (30 << PIO0_SM0_EXECCTRL_WRAP_TOP_Pos) |
                         (0 << PIO0_SM0_EXECCTRL_WRAP_BOTTOM_Pos);

    PIO0->SM0_SHIFTCTRL = PIO0_SM0_SHIFTCTRL_FJOIN_RX_Msk |
                          PIO0_SM0_SHIFTCTRL_AUTOPUSH_Msk |
                          (31 << PIO0_SM0_SHIFTCTRL_PUSH_THRESH_Pos);

    PIO0->SM0_PINCTRL = (DP_INDEX << PIO0_SM0_PINCTRL_IN_BASE_Pos);

    PIO0->SM0_INSTR = OP_JMP | JMP_ADDR(31);

    // PIO1 init
    PIO1->SM0_CLKDIV = ((g_buffer_info.fs ? 1 : 8) << PIO0_SM0_CLKDIV_INT_Pos);

    for (int i = 0; i < (int)(sizeof(pio1_ops) / sizeof(uint16_t)); i++)
        PIO1_INSTR_MEM[i] = pio1_ops[i];

    PIO1->SM0_EXECCTRL = (31 << PIO0_SM0_EXECCTRL_WRAP_TOP_Pos) |
                         (0 << PIO0_SM0_EXECCTRL_WRAP_BOTTOM_Pos);
    PIO1->SM0_SHIFTCTRL = 0;
    PIO1->SM0_PINCTRL = (DP_INDEX << PIO0_SM0_PINCTRL_IN_BASE_Pos) |
                        (START_INDEX << PIO0_SM0_PINCTRL_SET_BASE_Pos) |
                        (1 << PIO0_SM0_PINCTRL_SET_COUNT_Pos);

    PIO1->SM0_INSTR =
        OP_SET | SET_DST_PINDIRS | SET_DATA(1);  // Clear the START output
    PIO1->SM0_INSTR = OP_SET | SET_DST_PINS | SET_DATA(0);

    stream_packet_t current = {0};

    uint32_t captured = 0;
    uint32_t oversized = 0;

    bool too_long = false;

    uint32_t start_dropped = stream_queue_dropped();
    uint32_t start_consumed = stream_queue_consumed();
    uint32_t output_before_queued = stream_output_queued();
    uint32_t output_before_dropped = stream_output_dropped();
    uint32_t output_before_too_long = stream_output_too_long();
    stream_decode_stats_t before, after;
    stream_decode_get_stats(&before);

    set_error(false);

    if (!wait_for_trigger()) {
        display_puts("Capture stopped\r\n");
        return;
    }

    // エンドポイントの管理情報をリセット，Core 0が完了するまで待機
    uint32_t reset_id = stream_decode_request_endpoint_reset();
    uint32_t reset_deadline = TIMER->TIMELR + 1000000u;
    while (!stream_decode_endpoint_reset_complete(reset_id)) {
        if ((int32_t)(TIMER->TIMELR - reset_deadline) >= 0) {
            display_puts("Endpoint reset timeout\r\n");
            return;
        }
    }

    // Ensure the start message has completely left CDC before binary frames.
    display_puts("Capture started\r\n");
    uint32_t start_drain = stream_drain_request();
    while (!stream_drain_complete(start_drain)) { }

    PIO1_SET->CTRL = (1 << (PIO0_CTRL_SM_ENABLE_Pos + 0));
    PIO0_SET->CTRL = (1 << (PIO0_CTRL_SM_ENABLE_Pos + 0));

    while (1) {
        if (0 == (PIO0->FSTAT & (1 << (PIO0_FSTAT_RXEMPTY_Pos + 0)))) {
            uint32_t v = PIO0->RXF0;

            // 最上位bitが1 -> パケット終了
            if (v & 0x80000000) {
                current.bit_count = 0xffffffff - v;
                current.timestamp = TIMER->TIMELR;

                captured++;

                if (!too_long) {
                    // 完成したパケットをリングバッファに格納
                    // 満杯の場合は破棄
                    (void)stream_queue_push(&current);
                }
                else {
                    // 格納可能な最大長を超えたパケット
                    oversized++;
                }

                // 次パケットを受信する準備
                current.word_count = 0;
                too_long = false;

                // キャプチャ制限に達していたら終了
                if (g_buffer_info.limit != 0 && captured >= (uint32_t)g_buffer_info.limit)
                    break;

            } 
            else {
                // USBパケットの生データを受信
                if (!too_long) {
                    if (current.word_count < STREAM_MAX_WORDS)
                        current.words[current.word_count++] = v;
                    else
                        too_long = true;
                }
            }
        }

        if (poll_cmd() == 'p') break;
    }

    PIO0_CLR->CTRL = (1 << (PIO0_CTRL_SM_ENABLE_Pos + 0));
    PIO1_CLR->CTRL = (1 << (PIO0_CTRL_SM_ENABLE_Pos + 0));

    // Drain the queueand let Core 0 finish decoding before reporting.
    // The consumer runs continuously in main() on Core 0.
    uint32_t accepted = captured - oversized - (stream_queue_dropped() - start_dropped);
    uint32_t deadline = TIMER->TIMELR + 1000000u;  // 1 second timeout

    do {
        stream_decode_get_stats(&after);
        if ((uint32_t)(after.processed - before.processed) >= accepted) break;
    } while ((int32_t)(TIMER->TIMELR - deadline) < 0);

    stream_decode_get_stats(&after);

    // All binary frames, including the final partially filled CDC packet,
    // must complete before any human-readable statistics are queued.
    uint32_t stop_drain = stream_drain_request();
    while (!stream_drain_complete(stop_drain)) { }

    display_puts("\r\n");
    display_puts("Capture stopped: packets=");
    display_putdec(captured, 0);

    display_puts(" queue_dropped=");
    display_putdec(stream_queue_dropped() - start_dropped, 0);

    display_puts(" consumed=");
    display_putdec(stream_queue_consumed() - start_consumed, 0);

    display_puts(" oversized=");
    display_putdec(oversized, 0);

    display_puts(" pending=");
    display_putdec(stream_queue_pending(), 0);

    display_puts("\r\n");
    display_puts("Decoded=");
    display_putdec(after.processed - before.processed, 0);
    display_puts(" Valid=");
    display_putdec(after.valid - before.valid, 0);
    display_puts(" Errors=");
    display_putdec(after.errors - before.errors, 0);
    display_puts(" CRC=");
    display_putdec(after.crc - before.crc, 0);
    display_puts(" PID=");
    display_putdec(after.pid - before.pid, 0);
    display_puts(" STUFF=");
    display_putdec(after.stuff - before.stuff, 0);
    display_puts(" SYNC=");
    display_putdec(after.sync - before.sync, 0);
    display_puts(" NBIT=");
    display_putdec(after.nbit - before.nbit, 0);
    display_puts(" SIZE=");
    display_putdec(after.size - before.size, 0);
    display_puts("\r\n");
    display_puts("IN=");
    display_putdec(after.in - before.in, 0);
    display_puts(" DATA=");
    display_putdec(after.data - before.data, 0);
    display_puts(" SOF=");
    display_putdec(after.sof - before.sof, 0);
    display_puts("\r\n");
    display_puts(" MatchedIN_DATA=");
    display_putdec(after.matched_data - before.matched_data, 0);
    display_puts(" UnmatchedDATA=");
    display_putdec(after.unmatched_data - before.unmatched_data, 0);
    display_puts(" EndpointEvictions=");
    display_putdec(after.endpoint_evictions - before.endpoint_evictions, 0);
    display_puts("\r\n");

    stream_endpoint_stats_t endpoint_after[STREAM_ENDPOINT_SLOTS];
    uint32_t endpoint_after_count = stream_decode_get_endpoints(
        endpoint_after, STREAM_ENDPOINT_SLOTS);
    display_puts("OutputQueued=");
    display_putdec(stream_output_queued() - output_before_queued, 0);
    display_puts(" OutputDropped=");
    display_putdec(stream_output_dropped() - output_before_dropped, 0);
    display_puts(" OutputOversized=");
    display_putdec(stream_output_too_long() - output_before_too_long, 0);
    display_puts("\r\n");
    display_puts("Endpoint statistics (this capture):\r\n");
    for (uint32_t i = 0; i < endpoint_after_count; ++i) {
        const stream_endpoint_stats_t *entry = &endpoint_after[i];
        // Endpoint counters start at zero for this capture. Evicted entries
        // cannot be listed; their replacement count is reported separately.
        uint32_t in_count = entry->in_tokens;
        uint32_t data_count = entry->data_packets;
        uint32_t nak_count = entry->nak_packets;
        if (!in_count && !data_count && !nak_count) continue;
        display_puts("  Addr=");
        display_putdec(entry->addr, 0);
        display_puts(" EP=");
        display_putdec(entry->ep, 0);
        display_puts(" IN=");
        display_putdec(in_count, 0);
        display_puts(" DATA=");
        display_putdec(data_count, 0);
        display_puts(" NAK=");
        display_putdec(nak_count, 0);
        display_puts("\r\n");
    }
}

//-----------------------------------------------------------------------------
static void print_help(void) {
    display_puts(
        "\r\n------------------------------------------------------------------"
        "-\r\n");
    display_puts("USB Sniffer Lite. Built on " __DATE__ " " __TIME__ ".\r\n");
    display_puts("\r\n");
    display_puts("Settings:\r\n");
    display_puts("  e - Capture speed       : ");
    display_puts(capture_speed_str[g_capture_speed]);
    display_puts("\r\n");
    display_puts("  g - Capture trigger     : ");
    display_puts(capture_trigger_str[g_capture_trigger]);
    display_puts("\r\n");
    display_puts("  l - Capture limit       : ");
    display_puts(capture_limit_str[g_capture_limit]);
    display_puts("\r\n");
    display_puts("  t - Time display format : ");
    display_puts(display_time_str[g_display_time]);
    display_puts("\r\n");
    display_puts("  a - Data display format : ");
    display_puts(display_data_str[g_display_data]);
    display_puts("\r\n");
    display_puts("  f - Fold empty frames   : ");
    display_puts(display_fold_str[g_display_fold]);
    display_puts("\r\n");
    display_puts("\r\n");
    display_puts("Commands:\r\n");
    display_puts("  h - Print this help message\r\n");
    display_puts("  b - Display buffer\r\n");
    display_puts("  s - Start capture\r\n");
    display_puts("  p - Stop capture\r\n");
    display_puts("\r\n");
}

//-----------------------------------------------------------------------------
static void change_setting(char* name, int* value, int count,
                           const char* str[]) {
    (*value)++;

    if (*value == count) *value = 0;

    display_puts(name);
    display_puts(" changed to ");
    display_puts(str[*value]);
    display_puts("\r\n");
}

//-----------------------------------------------------------------------------
static void core1_main(void) {
    HAL_GPIO_TRIGGER_in();
    HAL_GPIO_TRIGGER_pullup();

    while (1) {
        int cmd = poll_cmd();

        if (cmd == 's')
            capture_buffer();
        else if (cmd == 'p') {
        }  // Do nothing here, stop only works if the capture is running
        else if (cmd == 'b')
            display_puts("Buffer display unavailable in streaming mode\r\n");
            // display_buffer();
        else if (cmd == 'h' || cmd == '?')
            print_help();
        else if (cmd == 'e')
            change_setting("Capture speed", &g_capture_speed, CaptureSpeedCount, capture_speed_str);
        else if (cmd == 'g')
            change_setting("Capture trigger", &g_capture_trigger, CaptureTriggerCount, capture_trigger_str);
        else if (cmd == 'l')
            change_setting("Capture limit", &g_capture_limit, CaptureLimitCount, capture_limit_str);
        else if (cmd == 't')
            change_setting("Time display format", &g_display_time, DisplayTimeCount, display_time_str);
        else if (cmd == 'a')
            change_setting("Data display format", &g_display_data, DisplayDataCount, display_data_str);
        else if (cmd == 'f')
            change_setting("Fold empty frames", &g_display_fold, DisplayFoldCount, display_fold_str);
    }
}

//-----------------------------------------------------------------------------
static void core1_start(void) {
    static uint32_t core1_stack[CORE1_STACK_SIZE];
    uint32_t* stack_ptr = core1_stack + CORE1_STACK_SIZE;
    const uint32_t cmd[] = {0, 1, (uint32_t)SCB->VTOR, (uint32_t)stack_ptr,
                            (uint32_t)core1_main};

    while (SIO->FIFO_ST & SIO_FIFO_ST_VLD_Msk) (void)SIO->FIFO_RD;

    __SEV();

    while (SIO->FIFO_ST & SIO_FIFO_ST_VLD_Msk) (void)SIO->FIFO_RD;

    for (int i = 0; i < (int)(sizeof(cmd) / sizeof(uint32_t)); i++) {
        SIO->FIFO_WR = cmd[i];
        __SEV();

        while (0 == (SIO->FIFO_ST & SIO_FIFO_ST_VLD_Msk));
        (void)SIO->FIFO_RD;
    }
}

//-----------------------------------------------------------------------------
void capture_init(void) { 
    core1_start(); 
}

//-----------------------------------------------------------------------------
void capture_command(int cmd) {
    if (SIO->FIFO_ST & SIO_FIFO_ST_RDY_Msk) SIO->FIFO_WR = cmd;
}
