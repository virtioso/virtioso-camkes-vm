/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "301/CO_driver.h"

void
CO_CANsetConfigurationMode(void *CANptr)
{
    (void)CANptr;
}

void
CO_CANsetNormalMode(CO_CANmodule_t *CANmodule)
{
    if (CANmodule == NULL || CANmodule->native_driver == NULL) {
        return;
    }

    if (canopen_native_driver_start(CANmodule->native_driver)) {
        CANmodule->CANnormal = true;
    }
}

CO_ReturnError_t
CO_CANmodule_init(CO_CANmodule_t *CANmodule, void *CANptr, CO_CANrx_t rxArray[],
                  uint16_t rxSize, CO_CANtx_t txArray[], uint16_t txSize,
                  uint16_t CANbitRate)
{
    uint16_t i;

    (void)CANbitRate;

    if (CANmodule == NULL || CANptr == NULL || rxArray == NULL || txArray == NULL) {
        return CO_ERROR_ILLEGAL_ARGUMENT;
    }

    memset(CANmodule, 0, sizeof(*CANmodule));
    CANmodule->CANptr = CANptr;
    CANmodule->native_driver = (canopen_native_driver_t *)CANptr;
    CANmodule->rxArray = rxArray;
    CANmodule->rxSize = rxSize;
    CANmodule->txArray = txArray;
    CANmodule->txSize = txSize;
    CANmodule->useCANrxFilters = false;
    CANmodule->firstCANtxMessage = true;

    for (i = 0U; i < rxSize; i++) {
        rxArray[i].ident = 0U;
        rxArray[i].mask = 0xFFFFU;
        rxArray[i].object = NULL;
        rxArray[i].CANrx_callback = NULL;
    }
    for (i = 0U; i < txSize; i++) {
        txArray[i].ident = 0U;
        txArray[i].DLC = 0U;
        memset(txArray[i].data, 0, sizeof(txArray[i].data));
        txArray[i].bufferFull = false;
        txArray[i].syncFlag = false;
    }

    return CO_ERROR_NO;
}

void
CO_CANmodule_disable(CO_CANmodule_t *CANmodule)
{
    if (CANmodule != NULL) {
        CANmodule->CANnormal = false;
    }
}

CO_ReturnError_t
CO_CANrxBufferInit(CO_CANmodule_t *CANmodule, uint16_t index, uint16_t ident,
                   uint16_t mask, bool_t rtr, void *object,
                   void (*CANrx_callback)(void *object, void *message))
{
    CO_CANrx_t *buffer;

    if (CANmodule == NULL || object == NULL || CANrx_callback == NULL || index >= CANmodule->rxSize) {
        return CO_ERROR_ILLEGAL_ARGUMENT;
    }

    buffer = &CANmodule->rxArray[index];
    buffer->object = object;
    buffer->CANrx_callback = CANrx_callback;
    buffer->ident = ident & 0x07FFU;
    if (rtr) {
        buffer->ident |= 0x0800U;
    }
    buffer->mask = (mask & 0x07FFU) | 0x0800U;

    return CO_ERROR_NO;
}

CO_CANtx_t *
CO_CANtxBufferInit(CO_CANmodule_t *CANmodule, uint16_t index, uint16_t ident,
                   bool_t rtr, uint8_t noOfBytes, bool_t syncFlag)
{
    CO_CANtx_t *buffer;

    if (CANmodule == NULL || index >= CANmodule->txSize) {
        return NULL;
    }

    buffer = &CANmodule->txArray[index];
    buffer->ident = ((uint32_t)ident & 0x07FFU) | (rtr ? 0x80000000U : 0U);
    buffer->DLC = noOfBytes > 8U ? 8U : noOfBytes;
    buffer->bufferFull = false;
    buffer->syncFlag = syncFlag;

    return buffer;
}

CO_ReturnError_t
CO_CANsend(CO_CANmodule_t *CANmodule, CO_CANtx_t *buffer)
{
    canopen_native_msg_t msg = {0};

    if (CANmodule == NULL || buffer == NULL || CANmodule->native_driver == NULL) {
        return CO_ERROR_ILLEGAL_ARGUMENT;
    }

    if (!CANmodule->CANnormal) {
        return CO_ERROR_INVALID_STATE;
    }

    if (buffer->bufferFull) {
        if (!CANmodule->firstCANtxMessage) {
            CANmodule->CANerrorStatus |= 0x0008U;
        }
        return CO_ERROR_TX_OVERFLOW;
    }

    msg.ident = buffer->ident & 0x07FFU;
    msg.DLC = buffer->DLC;
    memcpy(msg.data, buffer->data, sizeof(msg.data));

    if (!canopen_native_driver_send(CANmodule->native_driver, &msg)) {
        return CO_ERROR_TX_BUSY;
    }

    CANmodule->bufferInhibitFlag = buffer->syncFlag;
    CANmodule->firstCANtxMessage = false;
    return CO_ERROR_NO;
}

void
CO_CANclearPendingSyncPDOs(CO_CANmodule_t *CANmodule)
{
    uint32_t tpdoDeleted = 0U;

    if (CANmodule == NULL) {
        return;
    }

    if (CANmodule->bufferInhibitFlag) {
        CANmodule->bufferInhibitFlag = false;
        tpdoDeleted = 1U;
    }

    if (CANmodule->CANtxCount != 0U) {
        for (uint16_t i = 0; i < CANmodule->txSize; i++) {
            CO_CANtx_t *buffer = &CANmodule->txArray[i];
            if (buffer->bufferFull && buffer->syncFlag) {
                buffer->bufferFull = false;
                CANmodule->CANtxCount--;
                tpdoDeleted = 2U;
            }
        }
    }

    if (tpdoDeleted != 0U) {
        CANmodule->CANerrorStatus |= 0x0080U;
    }
}

void
CO_CANmodule_process(CO_CANmodule_t *CANmodule)
{
    if (CANmodule == NULL) {
        return;
    }

    CANmodule->errOld = CANmodule->CANerrorStatus;
}

void
CO_CANinterrupt(CO_CANmodule_t *CANmodule)
{
    canopen_native_msg_t rx_msg;

    if (CANmodule == NULL || CANmodule->native_driver == NULL || !CANmodule->CANnormal) {
        return;
    }

    if (!canopen_native_driver_poll(CANmodule->native_driver, &rx_msg)) {
        return;
    }

    CANmodule->last_rx_msg = rx_msg;
    CANmodule->has_last_rx_msg = true;

    for (uint16_t i = 0; i < CANmodule->rxSize; i++) {
        CO_CANrx_t *buffer = &CANmodule->rxArray[i];
        uint16_t rcv_ident = CO_CANrxMsg_readIdent(&rx_msg);

        if (((rcv_ident ^ buffer->ident) & buffer->mask) == 0U) {
            if (buffer->CANrx_callback != NULL) {
                buffer->CANrx_callback(buffer->object, &CANmodule->last_rx_msg);
            }
            break;
        }
    }
}
