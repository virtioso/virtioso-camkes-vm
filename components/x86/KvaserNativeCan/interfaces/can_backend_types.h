/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef KVASER_NATIVE_SNAPSHOT_T_DEFINED
#define KVASER_NATIVE_SNAPSHOT_T_DEFINED
typedef struct kvaser_native_snapshot {
    uint16_t vendor;
    uint16_t device;
    uint16_t command;
    uint16_t status;
    uint16_t class_code;
    uint8_t revision;
    uint8_t int_line;
    uint8_t int_pin;
    uint32_t bar0_cfg;
    uint32_t bar1_cfg;
    uint32_t bar2_cfg;
    uint32_t intcsr;
    uint32_t rcr;
    uint8_t sja_mod;
    uint8_t sja_sr;
    uint8_t sja_ir;
    uint8_t sja_ier;
    uint8_t sja_btr0;
    uint8_t sja_btr1;
    uint8_t sja_ocr;
    uint8_t sja_cdr;
    uint8_t xilinx_ver;
} kvaser_native_snapshot_t;
#endif

typedef struct can_backend_frame {
    uint32_t can_id;
    uint8_t dlc;
    uint8_t data[8];
    uint8_t extended;
    uint8_t rtr;
} can_backend_frame_t;

typedef struct can_backend_state {
    bool initialized;
    bool has_last_rx;
    uint32_t irq_count;
    uint32_t tx_request_count;
    uint32_t tx_complete_count;
    uint32_t rx_irq_count;
    uint32_t rx_frame_count;
    uint32_t data_overrun_count;
    uint32_t error_irq_count;
    uint8_t last_irq_bits;
    can_backend_frame_t last_rx_frame;
} can_backend_state_t;
