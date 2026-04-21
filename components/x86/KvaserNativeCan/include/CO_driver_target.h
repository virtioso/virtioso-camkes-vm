/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "canopen_native_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Force the upstream stack onto its static/global-object path.
 * This avoids heap-backed CO_new() in the native seL4 component. */
#define CO_USE_GLOBALS

#define CO_LITTLE_ENDIAN
#define CO_SWAP_16(x) (x)
#define CO_SWAP_32(x) (x)
#define CO_SWAP_64(x) (x)

typedef uint_fast8_t bool_t;
typedef float float32_t;
typedef double float64_t;

typedef canopen_native_msg_t CO_CANrxMsg_t;

static inline uint16_t
CO_CANrxMsg_readIdent(void *msg)
{
    return canopen_native_msg_read_ident((const canopen_native_msg_t *)msg);
}

static inline uint8_t
CO_CANrxMsg_readDLC(void *msg)
{
    return canopen_native_msg_read_dlc((const canopen_native_msg_t *)msg);
}

static inline const uint8_t *
CO_CANrxMsg_readData(void *msg)
{
    return canopen_native_msg_read_data((const canopen_native_msg_t *)msg);
}

typedef struct {
    uint16_t ident;
    uint16_t mask;
    void *object;
    void (*CANrx_callback)(void *object, void *message);
} CO_CANrx_t;

typedef struct {
    uint32_t ident;
    uint8_t DLC;
    uint8_t data[8];
    volatile bool_t bufferFull;
    volatile bool_t syncFlag;
} CO_CANtx_t;

typedef struct {
    void *CANptr;
    CO_CANrx_t *rxArray;
    uint16_t rxSize;
    CO_CANtx_t *txArray;
    uint16_t txSize;
    uint16_t CANerrorStatus;
    volatile bool_t CANnormal;
    volatile bool_t useCANrxFilters;
    volatile bool_t bufferInhibitFlag;
    volatile bool_t firstCANtxMessage;
    volatile uint16_t CANtxCount;
    uint32_t errOld;

    canopen_native_driver_t *native_driver;
    bool_t has_last_rx_msg;
    CO_CANrxMsg_t last_rx_msg;
} CO_CANmodule_t;

typedef struct {
    void *addr;
    size_t len;
    uint8_t subIndexOD;
    uint8_t attr;
    void *addrNV;
} CO_storage_entry_t;

#define CO_LOCK_CAN_SEND(CAN_MODULE)
#define CO_UNLOCK_CAN_SEND(CAN_MODULE)
#define CO_LOCK_EMCY(CAN_MODULE)
#define CO_UNLOCK_EMCY(CAN_MODULE)
#define CO_LOCK_OD(CAN_MODULE)
#define CO_UNLOCK_OD(CAN_MODULE)

#define CO_MemoryBarrier() __sync_synchronize()
#define CO_FLAG_READ(rxNew) ((rxNew) != NULL)
#define CO_FLAG_SET(rxNew) \
    do { \
        CO_MemoryBarrier(); \
        rxNew = (void *)1L; \
    } while (0)
#define CO_FLAG_CLEAR(rxNew) \
    do { \
        CO_MemoryBarrier(); \
        rxNew = NULL; \
    } while (0)

#ifdef __cplusplus
}
#endif
