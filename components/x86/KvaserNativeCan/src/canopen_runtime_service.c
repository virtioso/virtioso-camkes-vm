/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "301/CO_NMT_Heartbeat.h"
#include "301/CO_SYNC.h"
#include "OD.h"
#include "canopen_runtime_service.h"

#define CANOPEN_RUNTIME_SERVICE_NMT_CONTROL                                                                          \
    (CO_NMT_STARTUP_TO_OPERATIONAL | CO_NMT_ERR_ON_ERR_REG | CO_ERR_REG_GENERIC_ERR | CO_ERR_REG_COMMUNICATION)
#define CANOPEN_RUNTIME_SERVICE_FIRST_HB_TIME_MS        500U
#define CANOPEN_RUNTIME_SERVICE_SDO_SRV_TIMEOUT_MS      1000U
#define CANOPEN_RUNTIME_SERVICE_SDO_CLI_TIMEOUT_MS      500U
#define CANOPEN_RUNTIME_SERVICE_SDO_CLI_BLOCK_TRANSFER  false
#define CANOPEN_RUNTIME_SERVICE_OD_STATUS_BITS          NULL

static uint16_t canopen_runtime_service_status_word(
    const canopen_runtime_service_t *service)
{
    uint16_t status_word = 0;

    if (service->started) {
        status_word |= 1u << 0;
    }
    if (service->native_service.rx_frame_count != 0U) {
        status_word |= 1u << 1;
    }
    if (service->native_service.error_irq_count != 0U) {
        status_word |= 1u << 2;
    }

    return status_word;
}

static void canopen_runtime_service_refresh_state(
    canopen_runtime_service_t *service)
{
    CO_NMT_internalState_t nmt_state = CO_NMT_INITIALIZING;
    bool can_normal = false;
    canopen_runtime_state_t *state = &service->state;

    if (service == NULL) {
        return;
    }

    if (service->co != NULL && service->co->NMT != NULL) {
        nmt_state = CO_NMT_getInternalState(service->co->NMT);
    }
    if (service->co != NULL && service->co->CANmodule != NULL) {
        can_normal = service->co->CANmodule->CANnormal;
    }

    memset(state, 0, sizeof(*state));
    state->can_diag_valid = true;
    state->started = service->started;
    state->can_normal = can_normal;
    state->has_last_rx = service->native_service.has_last_rx;
    state->node_id = service->node_id;
    state->nmt_state = (uint8_t)nmt_state;
    state->status_word = canopen_runtime_service_status_word(service);
    state->can_tx_err_count = service->native_service.tx_complete_count;
    state->can_rx_err_count = service->native_service.rx_frame_count;
    state->can_bus_off_count = 0;
    state->can_overrun_count = service->native_service.data_overrun_count;
    state->cycle_count = service->process_count;
    state->uptime_ms = service->process_count;
    state->last_timer_next_us = service->last_timer_next_us;
    state->sequence = service->process_count;
}

void canopen_runtime_service_init(canopen_runtime_service_t *service)
{
    memset(service, 0, sizeof(*service));
    kvaser_native_can_service_init(&service->native_service);
    kvaser_canopen_port_init(&service->port, &service->native_service);
    canopen_native_driver_init(&service->native_driver, &service->port);
}

bool canopen_runtime_service_start(canopen_runtime_service_t *service,
                                   uint8_t node_id,
                                   uint16_t bitrate_kbit)
{
    uint32_t err_info = 0;
    CO_ReturnError_t err;

    if (service == NULL) {
        return false;
    }

    service->started = false;
    service->last_start_stage = 0;
    service->last_start_error = 0;
    service->last_err_info = 0;

    if (service->co == NULL) {
        service->last_start_stage = 1;
        service->co = CO_new(NULL, &service->heap_memory_used);
        if (service->co == NULL) {
            service->last_start_error = -1;
            return false;
        }
    }

    service->node_id = node_id;
    service->bitrate_kbit = bitrate_kbit;

    service->last_start_stage = 2;
    err = CO_CANinit(service->co, &service->native_driver, bitrate_kbit);
    if (err != CO_ERROR_NO) {
        service->last_start_error = err;
        return false;
    }

    service->last_start_stage = 3;
    err = CO_CANopenInit(service->co, NULL, NULL, OD,
                         CANOPEN_RUNTIME_SERVICE_OD_STATUS_BITS,
                         CANOPEN_RUNTIME_SERVICE_NMT_CONTROL,
                         CANOPEN_RUNTIME_SERVICE_FIRST_HB_TIME_MS,
                         CANOPEN_RUNTIME_SERVICE_SDO_SRV_TIMEOUT_MS,
                         CANOPEN_RUNTIME_SERVICE_SDO_CLI_TIMEOUT_MS,
                         CANOPEN_RUNTIME_SERVICE_SDO_CLI_BLOCK_TRANSFER,
                         node_id, &err_info);
    if (err != CO_ERROR_NO) {
        service->last_start_error = err;
        service->last_err_info = err_info;
        return false;
    }

    service->last_start_stage = 4;
    err = CO_CANopenInitPDO(service->co, service->co->em, OD, node_id,
                            &err_info);
    if (err != CO_ERROR_NO) {
        service->last_start_error = err;
        service->last_err_info = err_info;
        return false;
    }

    CO_CANsetNormalMode(service->co->CANmodule);
    service->started = service->co->CANmodule->CANnormal;
    service->last_start_stage = 5;
    service->last_start_error = service->started ? 0 : -2;
    service->last_reset = CO_RESET_NOT;
    canopen_runtime_service_refresh_state(service);
    return service->started;
}

void canopen_runtime_service_process(canopen_runtime_service_t *service,
                                     uint32_t time_difference_us)
{
    uint32_t timer_next_us = UINT32_MAX;
    bool_t sync_was;

    if (service == NULL || !service->started || service->co == NULL) {
        return;
    }

    service->last_reset = CO_process(service->co, false, time_difference_us,
                                     &timer_next_us);
    sync_was = CO_process_SYNC(service->co, time_difference_us, &timer_next_us);
    CO_process_RPDO(service->co, sync_was, time_difference_us, &timer_next_us);
    CO_process_TPDO(service->co, sync_was, time_difference_us, &timer_next_us);
    service->last_timer_next_us = timer_next_us;
    service->process_count++;
    canopen_runtime_service_refresh_state(service);
}

void canopen_runtime_service_handle_irq(canopen_runtime_service_t *service)
{
    if (service == NULL || !service->started || service->co == NULL) {
        return;
    }

    kvaser_native_can_service_handle_irq(&service->native_service);
    CO_CANinterrupt(service->co->CANmodule);
    canopen_runtime_service_refresh_state(service);
}

bool canopen_runtime_service_send_sync(canopen_runtime_service_t *service)
{
    if (service == NULL || !service->started || service->co == NULL ||
        service->co->SYNC == NULL) {
        return false;
    }

    return CO_SYNCsend(service->co->SYNC) == CO_ERROR_NO;
}

const canopen_runtime_state_t *canopen_runtime_service_state(
    const canopen_runtime_service_t *service)
{
    return service != NULL ? &service->state : NULL;
}
