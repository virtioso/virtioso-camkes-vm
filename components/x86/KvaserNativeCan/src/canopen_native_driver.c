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
    can_interface_client_init(&driver->can_interface_client);
}

bool canopen_native_driver_start(canopen_native_driver_t *driver)
{
    if (driver == NULL) {
        return false;
    }

    if (!can_interface_client_start(&driver->can_interface_client)) {
        return false;
    }

    driver->started = true;
    return true;
}

bool canopen_native_driver_send(canopen_native_driver_t *driver,
                                const canopen_native_msg_t *msg)
{
    can_interface_frame_t can_frame = {0};

    if (driver == NULL || msg == NULL || !driver->started) {
        return false;
    }

    can_frame.can_id = msg->can_id;
    can_frame.dlc = msg->DLC;
    memcpy(can_frame.data, msg->data, sizeof(can_frame.data));
    can_frame.extended = msg->extended ? 1U : 0U;
    can_frame.rtr = msg->rtr ? 1U : 0U;

    if (!can_interface_client_send(&driver->can_interface_client, &can_frame)) {
        return false;
    }

    driver->tx_messages++;
    return true;
}

bool canopen_native_driver_poll(canopen_native_driver_t *driver,
                                canopen_native_msg_t *msg)
{
    can_interface_frame_t can_frame = {0};

    if (driver == NULL || msg == NULL || !driver->started) {
        return false;
    }

    if (!can_interface_client_poll(&driver->can_interface_client, &can_frame)) {
        return false;
    }

    memset(msg, 0, sizeof(*msg));
    msg->can_id = can_frame.can_id;
    msg->DLC = can_frame.dlc;
    memcpy(msg->data, can_frame.data, sizeof(msg->data));
    msg->extended = can_frame.extended != 0U;
    msg->rtr = can_frame.rtr != 0U;

    driver->last_rx_msg = *msg;
    driver->has_last_rx = true;
    driver->rx_messages++;
    return true;
}
