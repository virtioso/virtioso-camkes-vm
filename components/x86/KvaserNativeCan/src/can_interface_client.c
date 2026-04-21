/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <camkes.h>
#include <string.h>

#include "can_interface_client.h"

void can_interface_client_init(can_interface_client_t *client)
{
    memset(client, 0, sizeof(*client));
}

bool can_interface_client_start(can_interface_client_t *client)
{
    if (client == NULL) {
        return false;
    }

    client->started = can_interface_start() != 0;
    if (client->started) {
        can_interface_client_refresh_state(client);
        can_interface_client_refresh_snapshot(client);
    }
    return client->started;
}

bool can_interface_client_send(can_interface_client_t *client,
                               const can_interface_frame_t *frame)
{
    if (client == NULL || frame == NULL || !client->started) {
        return false;
    }

    if (can_interface_send(*frame) == 0) {
        return false;
    }

    client->tx_messages++;
    return true;
}

bool can_interface_client_poll(can_interface_client_t *client,
                               can_interface_frame_t *frame)
{
    can_interface_frame_t polled = {0};

    if (client == NULL || frame == NULL || !client->started) {
        return false;
    }

    if (can_interface_poll(&polled) == 0) {
        return false;
    }

    *frame = polled;
    client->last_rx_frame = polled;
    client->has_last_rx = true;
    client->rx_messages++;
    return true;
}

void can_interface_client_refresh_state(can_interface_client_t *client)
{
    if (client == NULL) {
        return;
    }

    can_interface_get_state(&client->last_state);
}

void can_interface_client_refresh_snapshot(can_interface_client_t *client)
{
    if (client == NULL) {
        return;
    }

    can_interface_get_snapshot(&client->last_snapshot);
}
