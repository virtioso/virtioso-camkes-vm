/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <camkes.h>
#include <camkes/io.h>
#include <stdbool.h>
#include <stdint.h>

#include "kvaser_native.h"

#define KVASER_BUS 0x00
#define KVASER_DEV 0x04
#define KVASER_FUN 0x00

#define PCI_VENDOR_ID      0x00
#define PCI_DEVICE_ID      0x02
#define PCI_COMMAND        0x04
#define PCI_STATUS         0x06
#define PCI_REVISION_ID    0x08
#define PCI_CLASS_DEVICE   0x0a
#define PCI_BAR0           0x10
#define PCI_BAR1           0x14
#define PCI_BAR2           0x18
#define PCI_INTERRUPT_LINE 0x3c
#define PCI_INTERRUPT_PIN  0x3d

#define S5920_INTCSR       0x38
#define S5920_RCR          0x3c
#define S5920_PTCR         0x60
#define S5920_INT_ENABLE   0x00002000u

#define SJA_REG_MOD        0x00
#define SJA_REG_CMR        0x01
#define SJA_REG_SR         0x02
#define SJA_REG_IR         0x03
#define SJA_REG_IER        0x04
#define SJA_REG_BTR0       0x06
#define SJA_REG_BTR1       0x07
#define SJA_REG_OCR        0x08
#define SJA_REG_ECC        0x0c
#define SJA_REG_RXERR      0x0e
#define SJA_REG_TXERR      0x0f
#define SJA_REG_FRM        0x10
#define SJA_REG_ID1        0x11
#define SJA_REG_ID2        0x12
#define SJA_REG_DATA_SFF   0x13
#define SJA_REG_ACCC0      0x10
#define SJA_REG_ACCC1      0x11
#define SJA_REG_ACCC2      0x12
#define SJA_REG_ACCC3      0x13
#define SJA_REG_ACCM0      0x14
#define SJA_REG_ACCM1      0x15
#define SJA_REG_ACCM2      0x16
#define SJA_REG_ACCM3      0x17
#define SJA_REG_CDR        0x1f

#define SJA_MOD_RM         0x01
#define SJA_CMD_TR         0x01
#define SJA_IRQ_BEI        0x80
#define SJA_IRQ_ALL        0xff

/*
 * These values mirror the Linux kvaser_pci driver defaults in
 * drivers/net/can/sja1000/kvaser_pci.c.
 */
#define KVASER_PCI_OCR     0xda
#define KVASER_PCI_CDR     0xc7

static void sja_write(uint16_t reg, uint8_t value)
{
    bar1_out8_offset(reg, value);
}

static uint8_t sja_read(uint16_t reg)
{
    return bar1_in8_offset(reg);
}

static bool wait_for_mod_state(uint8_t expected_mask, uint8_t expected_value)
{
    for (int i = 0; i < 10000; i++) {
        if ((sja_read(SJA_REG_MOD) & expected_mask) == expected_value) {
            return true;
        }
    }

    return false;
}

static void enable_board_irq(void)
{
    uint32_t intcsr = bar0_in32_offset(S5920_INTCSR);
    bar0_out32_offset(S5920_INTCSR, intcsr | S5920_INT_ENABLE);
}

void kvaser_native_snapshot(kvaser_native_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    snapshot->vendor = pci_config_read16(KVASER_BUS, KVASER_DEV, KVASER_FUN, PCI_VENDOR_ID);
    snapshot->device = pci_config_read16(KVASER_BUS, KVASER_DEV, KVASER_FUN, PCI_DEVICE_ID);
    snapshot->command = pci_config_read16(KVASER_BUS, KVASER_DEV, KVASER_FUN, PCI_COMMAND);
    snapshot->status = pci_config_read16(KVASER_BUS, KVASER_DEV, KVASER_FUN, PCI_STATUS);
    snapshot->revision = pci_config_read8(KVASER_BUS, KVASER_DEV, KVASER_FUN, PCI_REVISION_ID);
    snapshot->class_code = pci_config_read16(KVASER_BUS, KVASER_DEV, KVASER_FUN, PCI_CLASS_DEVICE);
    snapshot->bar0_cfg = pci_config_read32(KVASER_BUS, KVASER_DEV, KVASER_FUN, PCI_BAR0);
    snapshot->bar1_cfg = pci_config_read32(KVASER_BUS, KVASER_DEV, KVASER_FUN, PCI_BAR1);
    snapshot->bar2_cfg = pci_config_read32(KVASER_BUS, KVASER_DEV, KVASER_FUN, PCI_BAR2);
    snapshot->int_line = pci_config_read8(KVASER_BUS, KVASER_DEV, KVASER_FUN, PCI_INTERRUPT_LINE);
    snapshot->int_pin = pci_config_read8(KVASER_BUS, KVASER_DEV, KVASER_FUN, PCI_INTERRUPT_PIN);

    snapshot->intcsr = bar0_in32_offset(S5920_INTCSR);
    snapshot->rcr = bar0_in32_offset(S5920_RCR);
    snapshot->sja_mod = bar1_in8_offset(SJA_REG_MOD);
    snapshot->sja_sr = bar1_in8_offset(SJA_REG_SR);
    /*
     * IR is destructive on read, so the native service owns consuming it in
     * IRQ context. Snapshots keep this field neutral to avoid stealing events
     * from the service path.
     */
    snapshot->sja_ir = 0;
    snapshot->sja_ier = bar1_in8_offset(SJA_REG_IER);
    snapshot->sja_btr0 = bar1_in8_offset(SJA_REG_BTR0);
    snapshot->sja_btr1 = bar1_in8_offset(SJA_REG_BTR1);
    snapshot->sja_ocr = bar1_in8_offset(SJA_REG_OCR);
    snapshot->sja_cdr = bar1_in8_offset(SJA_REG_CDR);
    snapshot->xilinx_ver = bar2_in8_offset(0);
}

bool kvaser_native_init_board(void)
{
    bar0_out32_offset(S5920_PTCR, 0x80808080u);
    enable_board_irq();

    sja_write(SJA_REG_IER, 0x00);
    sja_write(SJA_REG_MOD, SJA_MOD_RM);
    if (!wait_for_mod_state(SJA_MOD_RM, SJA_MOD_RM)) {
        return false;
    }

    sja_write(SJA_REG_CDR, KVASER_PCI_CDR);
    sja_write(SJA_REG_ACCC0, 0x00);
    sja_write(SJA_REG_ACCC1, 0x00);
    sja_write(SJA_REG_ACCC2, 0x00);
    sja_write(SJA_REG_ACCC3, 0x00);
    sja_write(SJA_REG_ACCM0, 0xff);
    sja_write(SJA_REG_ACCM1, 0xff);
    sja_write(SJA_REG_ACCM2, 0xff);
    sja_write(SJA_REG_ACCM3, 0xff);
    sja_write(SJA_REG_OCR, KVASER_PCI_OCR);

    sja_write(SJA_REG_BTR0, 0x00);
    sja_write(SJA_REG_BTR1, 0x14);
    sja_write(SJA_REG_TXERR, 0x00);
    sja_write(SJA_REG_RXERR, 0x00);
    (void)sja_read(SJA_REG_ECC);
    (void)sja_read(SJA_REG_IR);

    sja_write(SJA_REG_MOD, 0x00);
    if (!wait_for_mod_state(SJA_MOD_RM, 0x00)) {
        return false;
    }

    sja_write(SJA_REG_IER, SJA_IRQ_ALL & (uint8_t)~SJA_IRQ_BEI);
    return true;
}

void kvaser_native_send_test_frame(uint16_t can_id, uint8_t data0)
{
    sja_write(SJA_REG_FRM, 0x01);
    sja_write(SJA_REG_ID1, (uint8_t)(can_id >> 3));
    sja_write(SJA_REG_ID2, (uint8_t)((can_id & 0x7) << 5));
    sja_write(SJA_REG_DATA_SFF, data0);
    sja_write(SJA_REG_CMR, SJA_CMD_TR);
}
