/*
 * Copyright 2022, 2023, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Simple pl011 emulation for early guest debugging. To enable, add
 * "earlycon=pl011,mmio32,0x09000000" to kernel_bootcmdline.
 */

#include <sel4vm/guest_vm.h>
#include <sel4vm/guest_vcpu_fault.h>
#include <sel4vm/boot.h>
#include <sel4vm/guest_irq_controller.h>
#include <utils/util.h>

#include <vmlinux.h>

#include <virtioso/camkes/pl011.h>

#define PL011_UARTDR    0x00    /* UARTDR: uart data register */
#define PL011_UARTRSR   0x04    /* UARTRSR/UARTECR: receive status/error clear */
#define PL011_UARTFR    0x18    /* UARTFR: uart flag register */
#define PL011_UARTILPR  0x20    /* UARTILPR: IrDA low-power counter */
#define PL011_UARTIBRD  0x24    /* UARTIBRD: integer baud rate divisor */
#define PL011_UARTFBRD  0x28    /* UARTFBRD: fractional baud rate divisor */
#define PL011_UARTLCR_H 0x2c    /* UARTLCR_H: line control */
#define PL011_UARTCR    0x30    /* UARTCR: control */
#define PL011_UARTIFLS  0x34    /* UARTIFLS: interrupt FIFO level select */
#define PL011_UARTIMSC  0x38    /* UARTIMSC: interrupt mask set/clear */
#define PL011_UARTRIS   0x3c    /* UARTRIS: raw interrupt status */
#define PL011_UARTMIS   0x40    /* UARTMIS: masked interrupt status */
#define PL011_UARTICR   0x44    /* UARTICR: interrupt clear */
#define PL011_UARTDMACR 0x48    /* UARTDMACR: DMA control */

#define PL011_UARTFR_TXFE 0x80
#define PL011_UARTFR_RXFE 0x10

#define PL011_UARTINT_RX  BIT(4)
#define PL011_UARTINT_TX  BIT(5)
#define PL011_RX_BUFSIZE  (BIT(PAGE_BITS_4K) - 8)

typedef struct pl011_rx_buffer {
    uint32_t head;
    uint32_t tail;
    char buf[PL011_RX_BUFSIZE];
} pl011_rx_buffer_t;

extern void *serial_getchar_buf __attribute__((weak));
extern seL4_Word serial_getchar_notification_badge(void) __attribute__((weak));

static uint32_t pl011_id_read(uintptr_t offset)
{
    switch (offset) {
    case 0xfe0:
        return 0x11;
    case 0xfe4:
        return 0x10;
    case 0xfe8:
        return 0x14;
    case 0xfec:
        return 0x00;
    case 0xff0:
        return 0x0d;
    case 0xff4:
        return 0xf0;
    case 0xff8:
        return 0x05;
    case 0xffc:
        return 0xb1;
    default:
        return UINT32_MAX;
    }
}

__attribute__((weak)) void guest_putchar_putchar(int c)
{
    putchar(c);
}

static volatile pl011_rx_buffer_t *pl011_rx_buffer(void)
{
    if (&serial_getchar_buf == NULL || serial_getchar_buf == NULL) {
        return NULL;
    }

    return (volatile pl011_rx_buffer_t *)serial_getchar_buf;
}

static bool pl011_rx_available(void)
{
    volatile pl011_rx_buffer_t *rx = pl011_rx_buffer();

    return rx != NULL && rx->head != rx->tail;
}

static uint32_t pl011_rx_dequeue(void)
{
    volatile pl011_rx_buffer_t *rx = pl011_rx_buffer();
    uint32_t value;

    if (rx == NULL || rx->head == rx->tail) {
        return 0;
    }

    value = (uint8_t)rx->buf[rx->head];
    rx->head = (rx->head + 1) % sizeof(rx->buf);

    return value;
}

static uint32_t pl011_raw_interrupt_status(void)
{
    return pl011_rx_available() ? PL011_UARTINT_RX : 0;
}

static void pl011_update_irq(pl011_t *p)
{
    bool level;

    if (p->vm == NULL || p->irq == 0) {
        return;
    }

    level = (pl011_raw_interrupt_status() & p->imsc) != 0;
    if (level == p->irq_level) {
        return;
    }

    p->irq_level = level;
    int err = vm_set_irq_level(p->vm->vcpus[BOOT_VCPU], p->irq, level);
    ZF_LOGW_IF(err, "Failed to set PL011 IRQ %u level %u", p->irq, level);
}

static void pl011_irq_ack(vm_vcpu_t *vcpu, int irq UNUSED, void *cookie)
{
    pl011_t *p = cookie;

    p->irq_level = false;
    int err = vm_set_irq_level(vcpu, p->irq, false);
    ZF_LOGW_IF(err, "Failed to lower PL011 IRQ %u", p->irq);
    pl011_update_irq(p);
}

static int pl011_serial_rx_event(vm_t *vm UNUSED, void *cookie)
{
    pl011_update_irq(cookie);
    return 0;
}

static inline bool pl011_read_fault(pl011_t *p, vm_vcpu_t *vcpu,
                                    uintptr_t paddr, size_t len)
{
    uintptr_t offset = paddr - p->base;
    uint32_t id_value = pl011_id_read(offset);

    if (id_value != UINT32_MAX) {
        set_vcpu_fault_data(vcpu, id_value);
        return true;
    }

    switch (offset) {
    case PL011_UARTDR:
        set_vcpu_fault_data(vcpu, pl011_rx_dequeue());
        pl011_update_irq(p);
        break;
    case PL011_UARTRSR:
    case PL011_UARTILPR:
        set_vcpu_fault_data(vcpu, 0);
        break;
    case PL011_UARTFR:
        set_vcpu_fault_data(vcpu, PL011_UARTFR_TXFE |
                                  (pl011_rx_available() ? 0 : PL011_UARTFR_RXFE));
        break;
    case PL011_UARTIBRD:
        set_vcpu_fault_data(vcpu, p->ibrd);
        break;
    case PL011_UARTFBRD:
        set_vcpu_fault_data(vcpu, p->fbrd);
        break;
    case PL011_UARTLCR_H:
        set_vcpu_fault_data(vcpu, p->lcr_h);
        break;
    case PL011_UARTCR:
        set_vcpu_fault_data(vcpu, p->cr);
        break;
    case PL011_UARTIFLS:
        set_vcpu_fault_data(vcpu, p->ifls);
        break;
    case PL011_UARTIMSC:
        set_vcpu_fault_data(vcpu, p->imsc);
        break;
    case PL011_UARTRIS:
        set_vcpu_fault_data(vcpu, pl011_raw_interrupt_status());
        break;
    case PL011_UARTMIS:
        set_vcpu_fault_data(vcpu, pl011_raw_interrupt_status() & p->imsc);
        break;
    case PL011_UARTDMACR:
        set_vcpu_fault_data(vcpu, p->dmacr);
        break;
    default:
        ZF_LOGW("unhandled pl011 read: vcpu=%d addr=0x%"PRIxPTR" len=%zu",
                vcpu->vcpu_id, paddr, len);
        return false;
    }

    return true;
}

static inline bool pl011_write_fault(pl011_t *p, vm_vcpu_t *vcpu,
                                     uintptr_t paddr, size_t len)
{
    seL4_Word value = emulate_vcpu_fault(vcpu, 0);

    switch (paddr - p->base) {
    case PL011_UARTDR:
        guest_putchar_putchar((int)value);
        break;
    case PL011_UARTRSR:
    case PL011_UARTILPR:
        break;
    case PL011_UARTIBRD:
        p->ibrd = (uint32_t)value;
        break;
    case PL011_UARTFBRD:
        p->fbrd = (uint32_t)value;
        break;
    case PL011_UARTLCR_H:
        p->lcr_h = (uint32_t)value;
        break;
    case PL011_UARTCR:
        p->cr = (uint32_t)value;
        break;
    case PL011_UARTIFLS:
        p->ifls = (uint32_t)value;
        break;
    case PL011_UARTIMSC:
        p->imsc = (uint32_t)value;
        pl011_update_irq(p);
        break;
    case PL011_UARTICR:
        pl011_update_irq(p);
        break;
    case PL011_UARTDMACR:
        p->dmacr = (uint32_t)value;
        break;
    default:
        ZF_LOGW("unhandled pl011 write: vcpu=%d addr=0x%"PRIxPTR" len=%zu value=0x%"PRIxPTR,
                vcpu->vcpu_id, paddr, len, value);
        return false;
    }

    return true;
}

static memory_fault_result_t pl011_fault_handler(vm_t *vm, vm_vcpu_t *vcpu,
                                                 uintptr_t paddr, size_t len,
                                                 void *cookie)
{
    bool fault_handled;
    pl011_t *p = cookie;

    if (is_vcpu_read_fault(vcpu)) {
        fault_handled = pl011_read_fault(p, vcpu, paddr, len);
    } else {
        fault_handled = pl011_write_fault(p, vcpu, paddr, len);
    }

    if (!fault_handled) {
        return FAULT_UNHANDLED;
    }

    advance_vcpu_fault(vcpu);

    return FAULT_HANDLED;
}

void pl011_init(vm_t *vm, void *cookie)
{
    vm_memory_reservation_t *res;
    pl011_t *p = cookie;

    p->vm = vm;

    res = vm_reserve_memory_at(vm, p->base, p->size, pl011_fault_handler,
                               cookie);
    ZF_LOGF_IF(!res, "Cannot reserve range 0x%"PRIxPTR" - 0x%"PRIxPTR,
               p->base, p->base - 1 + p->size);

    if (p->irq != 0) {
        int err = vm_register_irq(vm->vcpus[BOOT_VCPU], p->irq, pl011_irq_ack, p);
        ZF_LOGF_IF(err, "Failed to register PL011 IRQ %u", p->irq);
    }

    if (serial_getchar_notification_badge != NULL) {
        int err = register_async_event_handler(serial_getchar_notification_badge(),
                                               pl011_serial_rx_event, p);
        ZF_LOGF_IF(err, "Failed to register PL011 serial input handler");
    }
}
