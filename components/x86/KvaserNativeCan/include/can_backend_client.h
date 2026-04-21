/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "can_backend_types.h"

typedef struct {
    bool started;
    uint32_t tx_messages;
    uint32_t rx_messages;
    bool has_last_rx;
    can_backend_frame_t last_rx_frame;
    can_backend_state_t last_state;
    kvaser_native_snapshot_t last_snapshot;
} can_backend_client_t;

void can_backend_client_init(can_backend_client_t *client);
bool can_backend_client_start(can_backend_client_t *client);
bool can_backend_client_send(can_backend_client_t *client,
                             const can_backend_frame_t *frame);
bool can_backend_client_poll(can_backend_client_t *client,
                             can_backend_frame_t *frame);
void can_backend_client_refresh_state(can_backend_client_t *client);
void can_backend_client_refresh_snapshot(can_backend_client_t *client);
