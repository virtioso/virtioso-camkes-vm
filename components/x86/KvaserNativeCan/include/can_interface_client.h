/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "can_interface_types.h"

typedef struct {
    bool started;
    uint32_t tx_messages;
    uint32_t rx_messages;
    bool has_last_rx;
    can_interface_frame_t last_rx_frame;
    can_interface_state_t last_state;
    kvaser_native_snapshot_t last_snapshot;
} can_interface_client_t;

void can_interface_client_init(can_interface_client_t *client);
bool can_interface_client_start(can_interface_client_t *client);
bool can_interface_client_send(can_interface_client_t *client,
                               const can_interface_frame_t *frame);
bool can_interface_client_poll(can_interface_client_t *client,
                               can_interface_frame_t *frame);
void can_interface_client_refresh_state(can_interface_client_t *client);
void can_interface_client_refresh_snapshot(can_interface_client_t *client);
