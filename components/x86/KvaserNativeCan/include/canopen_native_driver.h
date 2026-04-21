/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "kvaser_canopen_port.h"

typedef struct {
    uint32_t ident;
    uint8_t DLC;
    uint8_t padding[3];
    uint8_t data[8];
} canopen_native_msg_t;

static inline uint16_t canopen_native_msg_read_ident(const canopen_native_msg_t *msg)
{
    return (uint16_t)(msg->ident & 0x7ffU);
}

static inline uint8_t canopen_native_msg_read_dlc(const canopen_native_msg_t *msg)
{
    return msg->DLC;
}

static inline const uint8_t *canopen_native_msg_read_data(const canopen_native_msg_t *msg)
{
    return msg->data;
}

typedef struct {
    kvaser_canopen_port_t *port;
    bool started;
    uint32_t tx_messages;
    uint32_t rx_messages;
    bool has_last_rx;
    canopen_native_msg_t last_rx_msg;
} canopen_native_driver_t;

void canopen_native_driver_init(canopen_native_driver_t *driver,
                                kvaser_canopen_port_t *port);
bool canopen_native_driver_start(canopen_native_driver_t *driver);
bool canopen_native_driver_send(canopen_native_driver_t *driver,
                                const canopen_native_msg_t *msg);
bool canopen_native_driver_poll(canopen_native_driver_t *driver,
                                canopen_native_msg_t *msg);
