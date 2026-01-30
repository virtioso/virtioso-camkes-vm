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
