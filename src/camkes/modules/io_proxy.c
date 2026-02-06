/*
 * Copyright 2022, 2023, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <camkes.h>
#include <vmlinux.h>

#include <virtioso/fdt.h>
#include <virtioso/io_proxy.h>
#include <virtioso/camkes/io_proxy.h>
#include <virtioso/guest.h>

extern vka_t _vka; /* from CAmkES VM */

uintptr_t guest_ram_base;
size_t guest_ram_size;

void camkes_io_proxy_module_init(vm_t *vm, void *cookie)
{
    io_proxy_t *io_proxy = cookie;

    guest_ram_base = vm_config.ram.base;
    guest_ram_size = vm_config.ram.size;

    io_proxy->vka = &_vka;

    int err = libsel4vm_io_proxy_init(vm, io_proxy);
    if (err) {
        ZF_LOGF("libsel4vm_io_proxy_init() failed (%d)", err);
        /* no return */
    }
}
