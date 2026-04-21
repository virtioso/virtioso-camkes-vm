/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kvaser_native_can.h"

/*
 * Intentionally mirrors the CANopenLinux/CANopenNode CAN message shape:
 * 11-bit standard identifier in ident, DLC in bytes, 8-byte data payload.
 */
typedef struct kvaser_canopen_msg {
    uint32_t ident;
    uint8_t dlc;
    uint8_t padding[3];
    uint8_t data[8];
} kvaser_canopen_msg_t;

typedef struct kvaser_canopen_port {
    kvaser_native_can_service_t *service;
    bool started;
    uint32_t tx_messages;
    uint32_t rx_messages;
    uint32_t consumed_rx_frames;
    kvaser_canopen_msg_t last_rx_msg;
} kvaser_canopen_port_t;

void kvaser_canopen_port_init(kvaser_canopen_port_t *port,
                              kvaser_native_can_service_t *service);
bool kvaser_canopen_port_start(kvaser_canopen_port_t *port);
bool kvaser_canopen_port_send(kvaser_canopen_port_t *port,
                              const kvaser_canopen_msg_t *msg);
bool kvaser_canopen_port_poll_rx(kvaser_canopen_port_t *port,
                                 kvaser_canopen_msg_t *msg);
