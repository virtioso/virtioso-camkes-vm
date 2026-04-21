/*
 * Copyright 2023, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <virtioso/reservations.h>
#include <virtioso/list.h>
#include <sel4vmmplatsupport/pci_host_bridge.h>

#include <autoconf.h>

typedef struct mmio_reservation {
    uint64_t addr;
    uint64_t size;
    vm_memory_reservation_t *res;
    io_proxy_t *io_proxy;
} mmio_reservation_t;

static list_t mmio_reservations;

static int mmio_res_cmp(void *l, void *r)
{
    mmio_reservation_t *a = l;
    mmio_reservation_t *b = r;

    return !(a->addr == b->addr &&
             a->size == b->size &&
             a->io_proxy == b->io_proxy);
}

static inline mmio_reservation_t *mmio_res_find(io_proxy_t *io_proxy,
                                                uint64_t addr,
                                                uint64_t size)
{
    mmio_reservation_t res = {
        .addr = addr,
        .size = size,
        .io_proxy = io_proxy,
    };

    return list_item(&mmio_reservations, &res, &mmio_res_cmp);
}

/* Maximum MMIO region size (256MB) - reject larger regions from backend */
#define MAX_MMIO_REGION_SIZE (256ULL * 1024 * 1024)

static bool is_structural_pci_host_window(uint64_t addr, uint64_t size)
{
#if defined(CONFIG_PLAT_QEMU_ARM_VIRT)
    vmm_pci_host_bridge_t bridge;
    vmm_pci_host_bridge_init_qemu_arm_virt(&bridge);
    return vmm_pci_host_bridge_region_matches(bridge.config_region, addr, size) ||
           vmm_pci_host_bridge_region_matches(bridge.io_region, addr, size) ||
           vmm_pci_host_bridge_region_matches(bridge.mem32_region, addr, size) ||
           vmm_pci_host_bridge_region_matches(bridge.mem64_region, addr, size);
#else
    return false;
#endif
}

int mmio_res_assign(vm_t *vm, memory_fault_callback_fn fault_handler,
                    io_proxy_t *io_proxy, uint64_t addr, uint64_t size)
{
    vm_memory_reservation_t *res;

    if (is_structural_pci_host_window(addr, size)) {
        ZF_LOGI("Skipping structural PCI host aperture 0x%" PRIx64 " size 0x%"
                PRIx64 " for backend %p",
                addr, size, io_proxy);
        return 0;
    }

    /* Skip regions that are too large - QEMU may try to register huge PCI
     * memory windows (e.g., 64-bit BAR space at 0x8000000000). We can't
     * create vspace reservations that large, so just ignore them. The actual
     * device BARs will be registered as smaller regions. */
    if (size > MAX_MMIO_REGION_SIZE) {
        ZF_LOGW("Skipping oversized MMIO region 0x%" PRIx64 " size 0x%"
                PRIx64 " (max 0x%llx) for backend %p",
                addr, size, (unsigned long long)MAX_MMIO_REGION_SIZE, io_proxy);
        return 0;  /* Return success - this is expected for large PCI windows */
    }

    res = vm_reserve_memory_at(vm, addr, size, fault_handler, io_proxy);
    if (!res) {
        ZF_LOGE("Failed to reserve MMIO region 0x%" PRIx64 " size 0x%"
                PRIx64 " for backend %p", addr, size, io_proxy);
        return -1;
    }

    mmio_reservation_t *mmio = calloc(1, sizeof(*mmio));
    if (!mmio) {
        ZF_LOGE("Failed to allocate object for mmio reservation");
        return -1;
    }

    mmio->addr = addr;
    mmio->size = size;
    mmio->res = res;
    mmio->io_proxy = io_proxy;

    int err = list_append(&mmio_reservations, mmio);
    if (err) {
        ZF_LOGE("Failed to add mmio reservation to list");
        free(mmio);
    }

    return err;
}

int mmio_res_free(io_proxy_t *io_proxy, uint64_t addr, uint64_t size)
{
    mmio_reservation_t match = {
        .addr = addr,
        .size = size,
        .io_proxy = io_proxy,
    };

    mmio_reservation_t *res = list_item(&mmio_reservations, &match, &mmio_res_cmp);
    if (!res) {
        ZF_LOGE("Failed to find mmio reservation for 0x%" PRIx64 " size 0x%"
                PRIx64 " for backend %p", addr, size, io_proxy);
        return -1;
    }

    int err = list_remove(&mmio_reservations, &match, &mmio_res_cmp);
    ZF_LOGE_IF(err, "list_remove() failed");

    err = vm_reservation_free(res->res);
    ZF_LOGE_IF(err, "Failed to free mmio reservation 0x%" PRIx64 " size 0x%"
               PRIx64  "for backend %p", addr, size, io_proxy);
    free(res);

    return err;
}

int mmio_res_init(void)
{
    return list_init(&mmio_reservations);
}
