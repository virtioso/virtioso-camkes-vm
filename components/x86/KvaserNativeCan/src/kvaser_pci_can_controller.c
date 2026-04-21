/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <camkes.h>
#include <string.h>
#include <utils/util.h>
#include <utils/zf_log.h>

#include "can_interface_types.h"
#include "kvaser_native_can.h"

static kvaser_native_can_service_t backend_service;
static uint32_t consumed_rx_frames;

static void can_interface_frame_from_native(can_interface_frame_t *dst,
                                            const kvaser_native_can_frame_t *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->can_id = src->can_id;
    dst->dlc = src->dlc;
    memcpy(dst->data, src->data, sizeof(dst->data));
    dst->extended = src->extended ? 1U : 0U;
    dst->rtr = src->rtr ? 1U : 0U;
}

static void can_interface_state_from_service(can_interface_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->initialized = backend_service.initialized;
    state->has_last_rx = backend_service.has_last_rx;
    state->irq_count = backend_service.irq_count;
    state->tx_request_count = backend_service.tx_request_count;
    state->tx_complete_count = backend_service.tx_complete_count;
    state->rx_irq_count = backend_service.rx_irq_count;
    state->rx_frame_count = backend_service.rx_frame_count;
    state->data_overrun_count = backend_service.data_overrun_count;
    state->error_irq_count = backend_service.error_irq_count;
    state->last_irq_bits = backend_service.last_irq_bits;
    can_interface_frame_from_native(&state->last_rx_frame,
                                    &backend_service.last_rx_frame);
}

void pre_init(void)
{
    kvaser_native_can_service_init(&backend_service);
    consumed_rx_frames = 0;
}

int can_interface_start(void)
{
    if (!kvaser_native_can_service_start(&backend_service)) {
        return 0;
    }

    consumed_rx_frames = backend_service.rx_frame_count;
    return 1;
}

int can_interface_send(can_interface_frame_t frame)
{
    kvaser_native_can_frame_t native_frame = {0};

    native_frame.can_id = frame.can_id;
    native_frame.dlc = frame.dlc;
    memcpy(native_frame.data, frame.data, sizeof(native_frame.data));
    native_frame.extended = frame.extended != 0U;
    native_frame.rtr = frame.rtr != 0U;

    return kvaser_native_can_service_send(&backend_service, &native_frame) ? 1 : 0;
}

int can_interface_poll(can_interface_frame_t *frame)
{
    const kvaser_native_can_frame_t *last_rx;

    if (frame == NULL || !backend_service.has_last_rx) {
        return 0;
    }

    if (consumed_rx_frames == backend_service.rx_frame_count) {
        return 0;
    }

    last_rx = &backend_service.last_rx_frame;
    if (last_rx->extended || last_rx->rtr) {
        return 0;
    }

    can_interface_frame_from_native(frame, last_rx);
    consumed_rx_frames = backend_service.rx_frame_count;
    return 1;
}

void can_interface_get_state(can_interface_state_t *state)
{
    if (state == NULL) {
        return;
    }

    can_interface_state_from_service(state);
}

void can_interface_get_snapshot(kvaser_native_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    *snapshot = backend_service.last_snapshot;
}

void irq_handle(void)
{
    kvaser_native_can_service_handle_irq(&backend_service);
    int err = irq_acknowledge();
    ZF_LOGF_IF(err, "Failed to acknowledge Kvaser IRQ");
}
