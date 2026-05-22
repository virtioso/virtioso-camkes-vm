/*
 * Copyright 2022, 2023, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <configurations/vm.h>

#define VM_VIRTIOSO_INIT_DEF() \
    VM_INIT_DEF() \
    attribute int tracebuffer_base; \
    attribute int tracebuffer_size; \
    attribute int ramoops_base; \
    attribute int ramoops_size; \
    /* Timer interface for hyp_ftrace auto-dump timeout (seL4TimeServer has built-in notification) */ \
    maybe uses Timer hyp_ftrace_timer; \
    /* vm_virtio_driver_channels: This VM is the DRIVER in these virtio channels.
     * Uses virtio devices provided by another VM. */ \
    attribute { \
        int id; \
        string data_base; \
        string data_size; \
        string ctrl_base; \
        string ctrl_size; \
    } vm_virtio_driver_channels[] = []; \
    /* vm_virtio_device_channels: This VM is the DEVICE in these virtio channels.
     * Provides virtio backends (e.g., runs QEMU) to driver VMs. */ \
    attribute { \
        int id; \
        string data_base; \
        string data_size; \
        string ctrl_base; \
        string ctrl_size; \
    } vm_virtio_device_channels[] = []; \

#define VM_VIRTIOSO_CONFIGURATION_BASE_DEF(num) \
    vm##num.fs_shmem_size = 0x100000; \
    vm##num.global_endpoint_base = 1 << 27; \
    vm##num.asid_pool = true; \
    vm##num.simple = true; \
    vm##num.sem_value = 0; \

#define VM_VIRTIOSO_CONFIGURATION_DEF(num) \
    VM_VIRTIOSO_CONFIGURATION_BASE_DEF(num) \
    vm##num.guest_large_pages = true; \
    /* heap_size set per-app in camkes file - not in macro */

#define VM_VIRTIOSO_X86_CONFIGURATION_DEF(num) \
    VM_VIRTIOSO_CONFIGURATION_BASE_DEF(num) \
    vm##num.guest_large_pages = false; \
    /* heap_size set per-app in camkes file - not in macro */

#undef VM_COMPONENT_DEF
#define VM_COMPONENT_DEF(num) \
    component VM##num vm##num;

#define VIRTIO_COMPONENT_DEF(_num) \
    emits    VirtIONotify vm##_num##_ntfn_send; \
    consumes VirtIONotify vm##_num##_ntfn_recv; \
    dataport Buf(4096) vm##_num##_iobuf; \
    dataport Buf(4096) vm##_num##_memdev;

#define VIRTIO_DEVICE_COMPONENT_DEF(_num) \
    VIRTIO_COMPONENT_DEF(_num)

#define VIRTIO_DRIVER_COMPONENT_DEF(_num) \
    VIRTIO_COMPONENT_DEF(_num)

#define VIRTIO_COMPOSITION_DEF(_dev, _drv) \
    connection seL4SharedDataWithCaps vm##_dev##_vm##_drv##_iobuf(from vm##_dev.vm##_drv##_iobuf, to vm##_drv.vm##_dev##_iobuf); \
    connection seL4SharedDataWithCaps vm##_dev##_vm##_drv##_memdev(from vm##_dev.vm##_drv##_memdev, to vm##_drv.vm##_dev##_memdev); \
    connection seL4GlobalAsynch vm##_dev##_vm##_drv##_upcall(from vm##_drv.vm##_dev##_ntfn_send, to vm##_dev.vm##_drv##_ntfn_recv); \
    connection seL4Notification vm##_dev##_vm##_drv##_downcall(from vm##_dev.vm##_drv##_ntfn_send, to vm##_drv.vm##_dev##_ntfn_recv);

#define VIRTIO_CONFIGURATION_DEF(_dev, _drv) \
    vm##_dev##_vm##_drv##_memdev.base = VM##_dev##_VM##_drv##_VIRTIO_DATA_BASE; \
    vm##_dev##_vm##_drv##_memdev.size = VM##_dev##_VM##_drv##_VIRTIO_DATA_SIZE; \
    vm##_dev##_vm##_drv##_iobuf.size = VM##_dev##_VM##_drv##_VIRTIO_DATA_SIZE; \

/* VIRTIO_CHANNEL_DRIVER_CONFIGURATION_DEF: Config for the DRIVER side of a channel.
 * Use on VM that is the driver (uses virtio devices). */
#define VIRTIO_CHANNEL_DRIVER_CONFIGURATION_DEF(_dev, _drv) \
    { \
        "id" : _dev, \
        "data_base" : VAR_STRINGIZE(VM##_dev##_VM##_drv##_VIRTIO_DATA_BASE), \
        "data_size" : VAR_STRINGIZE(VM##_dev##_VM##_drv##_VIRTIO_DATA_SIZE), \
        "ctrl_base" : VAR_STRINGIZE(VM##_dev##_VM##_drv##_VIRTIO_CTRL_BASE), \
        "ctrl_size" : VAR_STRINGIZE(VM##_dev##_VM##_drv##_VIRTIO_CTRL_SIZE), \
    },

/* VIRTIO_CHANNEL_DEVICE_CONFIGURATION_DEF: Config for the DEVICE side of a channel.
 * Use on VM that is the device (provides virtio backends, e.g., runs QEMU). */
#define VIRTIO_CHANNEL_DEVICE_CONFIGURATION_DEF(_dev, _drv) \
    { \
        "id" : _drv, \
        "data_base" : VAR_STRINGIZE(VM##_dev##_VM##_drv##_VIRTIO_DATA_BASE), \
        "data_size" : VAR_STRINGIZE(VM##_dev##_VM##_drv##_VIRTIO_DATA_SIZE), \
        "ctrl_base" : VAR_STRINGIZE(VM##_dev##_VM##_drv##_VIRTIO_CTRL_BASE), \
        "ctrl_size" : VAR_STRINGIZE(VM##_dev##_VM##_drv##_VIRTIO_CTRL_SIZE), \
    },

#if VMSWIOTLB
#define VIRTIO_DRIVER_GUEST_RAM_CONFIGURATION_DEF(_num) \
        /* additional 16 MB pages for guest RAM */ \
        vm##_num.simple_untyped24_pool = 12 + (VM##_num##_RAM_SIZE >> 24);
#else
#define VIRTIO_DRIVER_GUEST_RAM_CONFIGURATION_DEF(_num) \
        /* this setting predates merging ARM VMM to camkes-vm repo */ \
        vm##_num.simple_untyped24_pool = 12; \
        /* guest RAM is mapped from virtio buffer dataport, hence we do not \
         * allocate untyped_mmios for it, nor do we increase \
         * simple_untyped24_pool. \
         */
#endif
