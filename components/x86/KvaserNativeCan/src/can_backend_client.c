/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <camkes.h>
#include <string.h>

#include "can_backend_client.h"

void can_backend_client_init(can_backend_client_t *client)
{
    memset(client, 0, sizeof(*client));
}

bool can_backend_client_start(can_backend_client_t *client)
{
    if (client == NULL) {
        return false;
    }

    client->started = can_backend_start() != 0;
    if (client->started) {
        can_backend_client_refresh_state(client);
        can_backend_client_refresh_snapshot(client);
    }
    return client->started;
}

bool can_backend_client_send(can_backend_client_t *client,
                             const can_backend_frame_t *frame)
{
    if (client == NULL || frame == NULL || !client->started) {
        return false;
    }

    if (can_backend_send(*frame) == 0) {
        return false;
    }

    client->tx_messages++;
    return true;
}

bool can_backend_client_poll(can_backend_client_t *client,
                             can_backend_frame_t *frame)
{
    can_backend_frame_t polled = {0};

    if (client == NULL || frame == NULL || !client->started) {
        return false;
    }

    if (can_backend_poll(&polled) == 0) {
        return false;
    }

    *frame = polled;
    client->last_rx_frame = polled;
    client->has_last_rx = true;
    client->rx_messages++;
    return true;
}

void can_backend_client_refresh_state(can_backend_client_t *client)
{
    if (client == NULL) {
        return;
    }

    can_backend_get_state(&client->last_state);
}

void can_backend_client_refresh_snapshot(can_backend_client_t *client)
{
    if (client == NULL) {
        return;
    }

    can_backend_get_snapshot(&client->last_snapshot);
}
