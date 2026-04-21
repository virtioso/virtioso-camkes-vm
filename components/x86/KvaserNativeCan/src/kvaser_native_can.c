/*
 * Copyright 2026, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <camkes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kvaser_native_can.h"

#define SJA_REG_CMR        0x01
#define SJA_REG_SR         0x02
#define SJA_REG_IR         0x03
#define SJA_REG_FI         0x10
#define SJA_REG_ID1        0x11
#define SJA_REG_ID2        0x12
#define SJA_REG_ID3        0x13
#define SJA_REG_ID4        0x14
#define SJA_REG_SFF_DATA   0x13
#define SJA_REG_EFF_DATA   0x15

#define SJA_CMD_RRB        0x04
#define SJA_CMD_TR         0x01

#define SJA_IRQ_BEI        0x80
#define SJA_IRQ_DOI        0x08
#define SJA_IRQ_EI         0x04
#define SJA_IRQ_TI         0x02
#define SJA_IRQ_RI         0x01

#define SJA_SR_RBS         0x01

#define SJA_FI_FF          0x80
#define SJA_FI_RTR         0x40
#define SJA_FI_DLC_MASK    0x0f

static inline uint8_t sja_read(uint16_t reg)
{
    return bar1_in8_offset(reg);
}

static inline void sja_write(uint16_t reg, uint8_t value)
{
    bar1_out8_offset(reg, value);
}

static void kvaser_native_can_snapshot(kvaser_native_can_service_t *service)
{
    kvaser_native_snapshot(&service->last_snapshot);
}

void kvaser_native_can_service_init(kvaser_native_can_service_t *service)
{
    memset(service, 0, sizeof(*service));
}

bool kvaser_native_can_service_start(kvaser_native_can_service_t *service)
{
    if (!kvaser_native_init_board()) {
        return false;
    }

    service->initialized = true;
    kvaser_native_can_snapshot(service);
    return true;
}

bool kvaser_native_can_service_send(kvaser_native_can_service_t *service,
                                    const kvaser_native_can_frame_t *frame)
{
    if (service == NULL || frame == NULL || !service->initialized) {
        return false;
    }

    if (frame->extended || frame->rtr || frame->dlc > 8) {
        return false;
    }

    service->tx_request_count++;
    kvaser_native_send_test_frame((uint16_t)(frame->can_id & 0x7ff), frame->data[0]);
    kvaser_native_can_snapshot(service);
    return true;
}

static void handle_rx_frame(kvaser_native_can_service_t *service)
{
    kvaser_native_can_frame_t frame = {0};
    uint8_t fi = sja_read(SJA_REG_FI);
    uint8_t dlc = fi & SJA_FI_DLC_MASK;

    frame.extended = (fi & SJA_FI_FF) != 0;
    frame.rtr = (fi & SJA_FI_RTR) != 0;
    frame.dlc = dlc > 8 ? 8 : dlc;

    if (!frame.extended) {
        uint8_t id1 = sja_read(SJA_REG_ID1);
        uint8_t id2 = sja_read(SJA_REG_ID2);
        frame.can_id = ((uint32_t)id1 << 3) | ((uint32_t)id2 >> 5);
        for (size_t i = 0; i < frame.dlc; i++) {
            frame.data[i] = sja_read(SJA_REG_SFF_DATA + i);
        }
    } else {
        uint8_t id1 = sja_read(SJA_REG_ID1);
        uint8_t id2 = sja_read(SJA_REG_ID2);
        uint8_t id3 = sja_read(SJA_REG_ID3);
        uint8_t id4 = sja_read(SJA_REG_ID4);
        frame.can_id = ((uint32_t)id1 << 21) |
                       ((uint32_t)id2 << 13) |
                       ((uint32_t)id3 << 5) |
                       ((uint32_t)id4 >> 3);
        for (size_t i = 0; i < frame.dlc; i++) {
            frame.data[i] = sja_read(SJA_REG_EFF_DATA + i);
        }
    }

    service->last_rx_frame = frame;
    service->has_last_rx = true;
    service->rx_frame_count++;
    sja_write(SJA_REG_CMR, SJA_CMD_RRB);
}

void kvaser_native_can_service_handle_irq(kvaser_native_can_service_t *service)
{
    uint8_t irq_bits;
    uint8_t status;

    if (service == NULL || !service->initialized) {
        return;
    }

    service->irq_count++;
    irq_bits = sja_read(SJA_REG_IR);
    status = sja_read(SJA_REG_SR);
    service->last_irq_bits = irq_bits;

    if (irq_bits & SJA_IRQ_TI) {
        service->tx_complete_count++;
    }
    if (irq_bits & SJA_IRQ_RI) {
        service->rx_irq_count++;
    }
    if (irq_bits & SJA_IRQ_DOI) {
        service->data_overrun_count++;
    }
    if (irq_bits & (SJA_IRQ_EI | SJA_IRQ_BEI)) {
        service->error_irq_count++;
    }

    if ((irq_bits & SJA_IRQ_RI) && (status & SJA_SR_RBS)) {
        handle_rx_frame(service);
    }

    kvaser_native_can_snapshot(service);
    service->last_snapshot.sja_ir = irq_bits;
}
