/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "canopen_native_driver.h"

void canopen_native_driver_init(canopen_native_driver_t *driver)
{
    memset(driver, 0, sizeof(*driver));
    can_backend_client_init(&driver->backend_client);
}

bool canopen_native_driver_start(canopen_native_driver_t *driver)
{
    if (driver == NULL) {
        return false;
    }

    if (!can_backend_client_start(&driver->backend_client)) {
        return false;
    }

    driver->started = true;
    return true;
}

bool canopen_native_driver_send(canopen_native_driver_t *driver,
                                const canopen_native_msg_t *msg)
{
    can_backend_frame_t backend_frame = {0};

    if (driver == NULL || msg == NULL || !driver->started) {
        return false;
    }

    backend_frame.can_id = msg->can_id;
    backend_frame.dlc = msg->DLC;
    memcpy(backend_frame.data, msg->data, sizeof(backend_frame.data));
    backend_frame.extended = msg->extended ? 1U : 0U;
    backend_frame.rtr = msg->rtr ? 1U : 0U;

    if (!can_backend_client_send(&driver->backend_client, &backend_frame)) {
        return false;
    }

    driver->tx_messages++;
    return true;
}

bool canopen_native_driver_poll(canopen_native_driver_t *driver,
                                canopen_native_msg_t *msg)
{
    can_backend_frame_t backend_frame = {0};

    if (driver == NULL || msg == NULL || !driver->started) {
        return false;
    }

    if (!can_backend_client_poll(&driver->backend_client, &backend_frame)) {
        return false;
    }

    memset(msg, 0, sizeof(*msg));
    msg->can_id = backend_frame.can_id;
    msg->DLC = backend_frame.dlc;
    memcpy(msg->data, backend_frame.data, sizeof(msg->data));
    msg->extended = backend_frame.extended != 0U;
    msg->rtr = backend_frame.rtr != 0U;

    driver->last_rx_msg = *msg;
    driver->has_last_rx = true;
    driver->rx_messages++;
    return true;
}
