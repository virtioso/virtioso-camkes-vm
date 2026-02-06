/*
 * Copyright 2023, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sel4vm/guest_irq_controller.h>

#include <virtioso/irq_line.h>
#include <virtioso/camkes/hyp_ftrace.h>

static void irq_line_ack(vm_vcpu_t *vcpu, int irq, void *cookie)
{
}

int irq_line_init(irq_line_t *line, vm_vcpu_t *vcpu, unsigned int irq,
                  void *cookie)
{
    line->vcpu = vcpu;
    line->irq = irq;
    line->cookie = cookie;

    int err = vm_register_irq(vcpu, irq, irq_line_ack, line);
    if (err) {
        ZF_LOGE("Failed to register IRQ %d (%d)", irq, err);
        return -1;
    }

    return 0;
}

int irq_line_change(irq_line_t *line, bool active)
{
    if (active) {
        hyp_ftrace_irq_injected();
    }
    return vm_set_irq_level(line->vcpu, line->irq, active);
}

int irq_line_pulse(irq_line_t *line)
{
    hyp_ftrace_irq_injected();

    int err = vm_set_irq_level(line->vcpu, line->irq, true);
    if (err) {
        return err;
    }
    return vm_set_irq_level(line->vcpu, line->irq, false);
}
