/*
 * Copyright 2026, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "canopen_native_driver.h"

void canopen_native_driver_init(canopen_native_driver_t *driver,
                                kvaser_canopen_port_t *port)
{
    memset(driver, 0, sizeof(*driver));
    driver->port = port;
}

bool canopen_native_driver_start(canopen_native_driver_t *driver)
{
    if (driver == NULL || driver->port == NULL) {
        return false;
    }

    if (!kvaser_canopen_port_start(driver->port)) {
        return false;
    }

    driver->started = true;
    return true;
}

bool canopen_native_driver_send(canopen_native_driver_t *driver,
                                const canopen_native_msg_t *msg)
{
    kvaser_canopen_msg_t port_msg = {0};

    if (driver == NULL || msg == NULL || !driver->started) {
        return false;
    }

    port_msg.ident = msg->ident;
    port_msg.dlc = msg->DLC;
    memcpy(port_msg.data, msg->data, sizeof(port_msg.data));

    if (!kvaser_canopen_port_send(driver->port, &port_msg)) {
        return false;
    }

    driver->tx_messages++;
    return true;
}

bool canopen_native_driver_poll(canopen_native_driver_t *driver,
                                canopen_native_msg_t *msg)
{
    kvaser_canopen_msg_t port_msg = {0};

    if (driver == NULL || msg == NULL || !driver->started) {
        return false;
    }

    if (!kvaser_canopen_port_poll_rx(driver->port, &port_msg)) {
        return false;
    }

    memset(msg, 0, sizeof(*msg));
    msg->ident = port_msg.ident;
    msg->DLC = port_msg.dlc;
    memcpy(msg->data, port_msg.data, sizeof(msg->data));

    driver->last_rx_msg = *msg;
    driver->has_last_rx = true;
    driver->rx_messages++;
    return true;
}
