/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "CANopen.h"
#include "canopen_native_driver.h"
#include "can_backend_types.h"
#include "kvaser_native.h"

typedef struct canopen_runtime_state {
    bool can_diag_valid;
    bool started;
    bool can_normal;
    bool has_last_rx;
    uint8_t node_id;
    uint8_t nmt_state;
    uint16_t status_word;
    uint32_t can_tx_err_count;
    uint32_t can_rx_err_count;
    uint32_t can_bus_off_count;
    uint32_t can_overrun_count;
    uint32_t cycle_count;
    uint32_t uptime_ms;
    uint32_t last_timer_next_us;
    uint64_t sequence;
} canopen_runtime_state_t;

typedef struct {
    canopen_native_driver_t native_driver;
    CO_t *co;
    bool started;
    uint8_t node_id;
    uint16_t bitrate_kbit;
    uint32_t process_count;
    uint32_t heap_memory_used;
    uint32_t last_timer_next_us;
    uint32_t last_err_info;
    int last_start_stage;
    int last_start_error;
    CO_NMT_reset_cmd_t last_reset;
    can_backend_state_t backend_state;
    kvaser_native_snapshot_t backend_snapshot;
    canopen_runtime_state_t state;
} canopen_runtime_service_t;

void canopen_runtime_service_init(canopen_runtime_service_t *service);
bool canopen_runtime_service_start(canopen_runtime_service_t *service,
                                   uint8_t node_id,
                                   uint16_t bitrate_kbit);
void canopen_runtime_service_process(canopen_runtime_service_t *service,
                                     uint32_t time_difference_us);
bool canopen_runtime_service_send_sync(canopen_runtime_service_t *service);
const canopen_runtime_state_t *canopen_runtime_service_state(
    const canopen_runtime_service_t *service);
