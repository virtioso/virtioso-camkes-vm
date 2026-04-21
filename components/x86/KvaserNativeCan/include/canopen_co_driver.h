/*
 * Copyright 2026, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "CO_driver_target.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CO_ERROR_NO = 0,
    CO_ERROR_ILLEGAL_ARGUMENT = -1,
    CO_ERROR_OUT_OF_MEMORY = -2,
    CO_ERROR_TIMEOUT = -3,
    CO_ERROR_ILLEGAL_BAUDRATE = -4,
    CO_ERROR_RX_OVERFLOW = -5,
    CO_ERROR_RX_PDO_OVERFLOW = -6,
    CO_ERROR_RX_MSG_LENGTH = -7,
    CO_ERROR_RX_PDO_LENGTH = -8,
    CO_ERROR_TX_OVERFLOW = -9,
    CO_ERROR_TX_PDO_WINDOW = -10,
    CO_ERROR_TX_UNCONFIGURED = -11,
    CO_ERROR_OD_PARAMETERS = -12,
    CO_ERROR_DATA_CORRUPT = -13,
    CO_ERROR_CRC = -14,
    CO_ERROR_TX_BUSY = -15,
    CO_ERROR_WRONG_NMT_STATE = -16,
    CO_ERROR_SYSCALL = -17,
    CO_ERROR_INVALID_STATE = -18,
    CO_ERROR_NODE_ID_UNCONFIGURED_LSS = -19
} CO_ReturnError_t;

void CO_CANsetConfigurationMode(void *CANptr);
void CO_CANsetNormalMode(CO_CANmodule_t *CANmodule);
CO_ReturnError_t CO_CANmodule_init(CO_CANmodule_t *CANmodule, void *CANptr,
                                   CO_CANrx_t rxArray[], uint16_t rxSize,
                                   CO_CANtx_t txArray[], uint16_t txSize,
                                   uint16_t CANbitRate);
void CO_CANmodule_disable(CO_CANmodule_t *CANmodule);
CO_ReturnError_t CO_CANrxBufferInit(CO_CANmodule_t *CANmodule, uint16_t index,
                                    uint16_t ident, uint16_t mask, bool_t rtr,
                                    void *object,
                                    void (*CANrx_callback)(void *object, void *message));
CO_CANtx_t *CO_CANtxBufferInit(CO_CANmodule_t *CANmodule, uint16_t index,
                               uint16_t ident, bool_t rtr, uint8_t noOfBytes,
                               bool_t syncFlag);
CO_ReturnError_t CO_CANsend(CO_CANmodule_t *CANmodule, CO_CANtx_t *buffer);
void CO_CANclearPendingSyncPDOs(CO_CANmodule_t *CANmodule);
void CO_CANmodule_process(CO_CANmodule_t *CANmodule);
void CO_CANinterrupt(CO_CANmodule_t *CANmodule);

#ifdef __cplusplus
}
#endif
