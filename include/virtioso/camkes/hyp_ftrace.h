/*
 * Copyright 2024, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hypervisor Ftrace Control Interface
 */

#pragma once

#include <sel4vm/guest_vm.h>

typedef struct hyp_ftrace {
    uintptr_t base;
    size_t size;
} hyp_ftrace_t;

void hyp_ftrace_init(vm_t *vm, void *cookie);

/**
 * Notify hyp_ftrace that an interrupt was injected to guest.
 * Resets the auto-dump timeout if armed.
 */
void hyp_ftrace_irq_injected(void);

/**
 * Handle timer notification for auto-dump timeout check.
 * Called every 1 second when timer is registered.
 * Triggers auto-dump if armed and no IRQs for 5 seconds.
 */
void hyp_ftrace_handle_timer_notification(void);
