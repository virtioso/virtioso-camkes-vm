/*
 * Copyright 2026, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kvaser_native.h"

typedef struct kvaser_native_can_frame {
    uint32_t can_id;
    uint8_t dlc;
    uint8_t data[8];
    bool extended;
    bool rtr;
} kvaser_native_can_frame_t;

typedef struct kvaser_native_can_service {
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
    kvaser_native_snapshot_t last_snapshot;
    kvaser_native_can_frame_t last_rx_frame;
} kvaser_native_can_service_t;

void kvaser_native_can_service_init(kvaser_native_can_service_t *service);
bool kvaser_native_can_service_start(kvaser_native_can_service_t *service);
bool kvaser_native_can_service_send(kvaser_native_can_service_t *service,
                                    const kvaser_native_can_frame_t *frame);
void kvaser_native_can_service_handle_irq(kvaser_native_can_service_t *service);
