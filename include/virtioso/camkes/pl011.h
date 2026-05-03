/*
 * Copyright 2024, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sel4vm/guest_vm.h>

typedef struct pl011 {
    vm_t *vm;
    uintptr_t base;
    size_t size;
    uint32_t irq;
    uint32_t cr;
    uint32_t ibrd;
    uint32_t fbrd;
    uint32_t lcr_h;
    uint32_t ifls;
    uint32_t imsc;
    uint32_t dmacr;
    bool irq_level;
} pl011_t;

void pl011_init(vm_t *vm, void *cookie);
