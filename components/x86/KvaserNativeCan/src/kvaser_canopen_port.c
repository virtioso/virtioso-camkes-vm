/*
 * Copyright 2026, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "kvaser_canopen_port.h"

void kvaser_canopen_port_init(kvaser_canopen_port_t *port,
                              kvaser_native_can_service_t *service)
{
    memset(port, 0, sizeof(*port));
    port->service = service;
}

bool kvaser_canopen_port_start(kvaser_canopen_port_t *port)
{
    if (port == NULL || port->service == NULL) {
        return false;
    }

    if (!kvaser_native_can_service_start(port->service)) {
        return false;
    }

    port->started = true;
    port->consumed_rx_frames = port->service->rx_frame_count;
    return true;
}

bool kvaser_canopen_port_send(kvaser_canopen_port_t *port,
                              const kvaser_canopen_msg_t *msg)
{
    kvaser_native_can_frame_t frame = {0};

    if (port == NULL || msg == NULL || !port->started) {
        return false;
    }

    if (msg->ident > 0x7ff || msg->dlc > 8) {
        return false;
    }

    frame.can_id = msg->ident;
    frame.dlc = msg->dlc;
    memcpy(frame.data, msg->data, sizeof(frame.data));
    frame.extended = false;
    frame.rtr = false;

    if (!kvaser_native_can_service_send(port->service, &frame)) {
        return false;
    }

    port->tx_messages++;
    return true;
}

bool kvaser_canopen_port_poll_rx(kvaser_canopen_port_t *port,
                                 kvaser_canopen_msg_t *msg)
{
    const kvaser_native_can_frame_t *frame;

    if (port == NULL || msg == NULL || !port->started) {
        return false;
    }

    if (!port->service->has_last_rx) {
        return false;
    }

    if (port->consumed_rx_frames == port->service->rx_frame_count) {
        return false;
    }

    frame = &port->service->last_rx_frame;
    if (frame->extended || frame->rtr) {
        return false;
    }

    memset(msg, 0, sizeof(*msg));
    msg->ident = frame->can_id;
    msg->dlc = frame->dlc;
    memcpy(msg->data, frame->data, sizeof(msg->data));

    port->last_rx_msg = *msg;
    port->consumed_rx_frames = port->service->rx_frame_count;
    port->rx_messages++;
    return true;
}
