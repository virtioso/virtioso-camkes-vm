/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2022, 2023, Unikie
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <camkes.h>
#include <vmlinux.h>
#include <sel4vm/guest_vm.h>
#include <inttypes.h>
#include <libfdt.h>

#include <sel4vmmplatsupport/drivers/cross_vm_connection.h>
#include <virtioso/backend/dt.h>
#include <virtioso/fdt.h>

#ifdef CONFIG_PLAT_QEMU_ARM_VIRT
#define CONNECTION_BASE_ADDRESS 0xC0000000
#elif CONFIG_PLAT_BCM2711
#define CONNECTION_BASE_ADDRESS 0x60000000
#elif CONFIG_PLAT_ORIN_AGX
/* Must be within PCI_MEM_REGION (0xC1000000+), after SWIOTLB (0xC0000000-0xC07FFFFF) */
#define CONNECTION_BASE_ADDRESS 0xC1000000
#elif defined(CONFIG_ARCH_X86)
/*
 * Keep the synthetic cross-VM PCI BARs out of the physical PCI host-bridge
 * passthrough window; the x86 q35 VPCI root window advertises this range.
 */
#define CONNECTION_BASE_ADDRESS 0xA0000000
#else
#define CONNECTION_BASE_ADDRESS 0x3F000000
#endif

/*- set vm_virtio_device_channels = configuration[me.name].get('vm_virtio_device_channels') -*/
/*- for drv in vm_virtio_device_channels -*/
extern dataport_caps_handle_t vm/*? drv.id ?*/_iobuf_handle;
extern dataport_caps_handle_t vm/*? drv.id ?*/_memdev_handle;

extern seL4_Word vm/*? drv.id ?*/_ntfn_recv_notification_badge(void);
/*- endfor -*/

static struct camkes_crossvm_connection connections[] = {
/*- for drv in vm_virtio_device_channels -*/
    {
        &vm/*? drv.id ?*/_memdev_handle,
        vm/*? drv.id ?*/_ntfn_send_emit,
        -1,
        "guest-device-/*? drv.id ?*/",
        &vm/*? drv.id ?*/_iobuf_handle
    },
/*- endfor -*/
};

typedef struct fdt_sel4_camkes_rpc {
    fdt_node_t node;
    uintptr_t event_base;
    uintptr_t data_base;
    uintptr_t ctrl_base;
    size_t bar_size;
    uint32_t irq_spi;
    uint32_t driver_vmid;
} fdt_sel4_camkes_rpc_t;

#ifdef CONFIG_ARCH_ARM
static int fdt_node_generate_sel4_camkes_rpc(fdt_node_t *node, void *fdt)
{
    fdt_sel4_camkes_rpc_t *rpc = (fdt_sel4_camkes_rpc_t *)node;
    int root = fdt_path_offset(fdt, "/");
    if (root < 0) {
        ZF_LOGE("fdt_path_offset(/) failed (%d)", root);
        return root;
    }

    char name[64];
    int n = snprintf(name, sizeof(name), "%s@%" PRIxPTR,
                     VIRTIOSO_DT_RPC_NODE_PREFIX, rpc->event_base);
    if (n < 0 || n >= (int)sizeof(name)) {
        return -FDT_ERR_INTERNAL;
    }

    int off = fdt_add_subnode(fdt, root, name);
    if (off == -FDT_ERR_EXISTS) {
        return 0;
    }
    if (off < 0) {
        ZF_LOGE("fdt_add_subnode(%s) failed (%d)", name, off);
        return off;
    }

    int err = fdt_setprop_string(fdt, off, "compatible", VIRTIOSO_DT_RPC_COMPATIBLE);
    if (err) {
        return err;
    }

    uint64_t reg[] = {
        cpu_to_fdt64(rpc->event_base), cpu_to_fdt64(rpc->bar_size),
        cpu_to_fdt64(rpc->data_base),  cpu_to_fdt64(rpc->bar_size),
        cpu_to_fdt64(rpc->ctrl_base),  cpu_to_fdt64(rpc->bar_size),
    };
    err = fdt_setprop(fdt, off, "reg", reg, sizeof(reg));
    if (err) {
        return err;
    }

    uint32_t interrupts[] = {
        cpu_to_fdt32(0), /* GIC_SPI */
        cpu_to_fdt32(rpc->irq_spi),
        cpu_to_fdt32(4), /* IRQ_TYPE_LEVEL_HIGH */
    };
    err = fdt_setprop(fdt, off, "interrupts", interrupts, sizeof(interrupts));
    if (err) {
        return err;
    }

    err = fdt_setprop_u32(fdt, off, VIRTIOSO_DT_DRIVER_VM_ID_PROPERTY, rpc->driver_vmid);
    if (err) {
        return err;
    }

    err = fdt_setprop_string(fdt, off, "status", "okay");
    if (err) {
        return err;
    }

    node->generated = true;
    return 0;
}

/*- for drv in vm_virtio_device_channels -*/
static fdt_sel4_camkes_rpc_t fdt_sel4_camkes_rpc_vm/*? drv.id ?*/ = {
    .node = {
        .name = VIRTIOSO_DT_RPC_NODE_PREFIX,
        .compatible = VIRTIOSO_DT_RPC_COMPATIBLE,
        .generate = fdt_node_generate_sel4_camkes_rpc,
    },
    .event_base = CONNECTION_BASE_ADDRESS + ((/*? loop.index0 ?*/) * (3ULL * (/*? drv.data_size ?*/))),
    .data_base = CONNECTION_BASE_ADDRESS + ((/*? loop.index0 ?*/) * (3ULL * (/*? drv.data_size ?*/))) + (/*? drv.data_size ?*/),
    .ctrl_base = CONNECTION_BASE_ADDRESS + ((/*? loop.index0 ?*/) * (3ULL * (/*? drv.data_size ?*/))) + (2ULL * (/*? drv.data_size ?*/)),
    .bar_size = /*? drv.data_size ?*/,
    .irq_spi = free_plat_interrupts[0] - IRQ_SPI_OFFSET,
    .driver_vmid = /*? drv.id ?*/,
};

DEFINE_FDT_NODE(fdt_sel4_camkes_rpc_vm/*? drv.id ?*/, &fdt_sel4_camkes_rpc_vm/*? drv.id ?*/.node)
/*- endfor -*/
#endif

static int consume_callback(vm_t *vm, void *cookie)
{
    struct camkes_crossvm_connection *connection = cookie;
#ifdef CONFIG_VIRTIO_VM_DEBUG
    ZF_LOGE("consume_callback: badge=%lu name=%s",
            (unsigned long)connection->consume_badge,
            connection->connection_name ? connection->connection_name : "(null)");
#endif
    consume_connection_event(vm, connection->consume_badge, true);
    return 0;
}

static void init_cross_vm_connections(vm_t *vm, void *cookie)
{
    int err;

/*- for drv in vm_virtio_device_channels -*/
    connections[/*? loop.index0 ?*/].consume_badge = vm/*? drv.id ?*/_ntfn_recv_notification_badge();
    err = register_async_event_handler(connections[/*? loop.index0 ?*/].consume_badge, consume_callback, &connections[/*? loop.index0 ?*/]);
    ZF_LOGF_IF(err, "Failed to register_async_event_handler for init_cross_vm_connections.");
/*- endfor -*/

    err = cross_vm_connections_init(vm, CONNECTION_BASE_ADDRESS, connections, ARRAY_SIZE(connections));
    ZF_LOGF_IF(err, "init_cross_vm_connections() failed");
}

/*- if vm_virtio_device_channels|length > 0 -*/
DEFINE_MODULE(cross_vm_connections, NULL, init_cross_vm_connections)
DEFINE_MODULE_DEP(cross_vm_connections, vpci_init)
DEFINE_MODULE_DEP(vpci_register_devices, cross_vm_connections)
/*- endif -*/

const char *append_vm_virtio_device_cmdline(char *buffer)
{
/*- if vm_virtio_device_channels|length > 0 -*/
    unsigned int id;
    uintptr_t data_base, ctrl_base;
    size_t data_size, ctrl_size;
    char *p = buffer;
/*- endif -*/

/*- for drv in vm_virtio_device_channels -*/
    id = /*? drv.id ?*/;
    data_base = /*? drv.data_base ?*/;
    data_size = /*? drv.data_size ?*/;
    ctrl_base = /*? drv.ctrl_base ?*/;
    ctrl_size = /*? drv.ctrl_size ?*/;
    /* TODO: safety checks */
    p += strlen(p);
    sprintf(p, " uservm=%u,0x%"PRIxPTR",0x%zx,0x%"PRIxPTR",0x%zx", id,
            data_base, data_size, ctrl_base, ctrl_size);
/*- endfor -*/

    return buffer;
}
