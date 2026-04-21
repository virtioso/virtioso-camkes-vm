/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

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

void kvaser_native_snapshot(kvaser_native_snapshot_t *snapshot);
bool kvaser_native_init_board(void);
void kvaser_native_send_test_frame(uint16_t can_id, uint8_t data0);
