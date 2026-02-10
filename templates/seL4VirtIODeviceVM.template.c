/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2022, 2023, Unikie
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <camkes.h>
#include <vmlinux.h>
#include <sel4vm/guest_vm.h>

#include <sel4vmmplatsupport/drivers/cross_vm_connection.h>

#ifdef CONFIG_PLAT_QEMU_ARM_VIRT
#define CONNECTION_BASE_ADDRESS 0xC0000000
#elif CONFIG_PLAT_BCM2711
#define CONNECTION_BASE_ADDRESS 0x60000000
#elif CONFIG_PLAT_ORIN_AGX
/* Must be within PCI_MEM_REGION (0xC1000000+), after SWIOTLB (0xC0000000-0xC07FFFFF) */
#define CONNECTION_BASE_ADDRESS 0xC1000000
#else
#define CONNECTION_BASE_ADDRESS 0x3F000000
#endif

#define DEBUG_VIRTIO_CONSUME_PATH

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

static int consume_callback(vm_t *vm, void *cookie)
{
    struct camkes_crossvm_connection *connection = cookie;
#ifdef DEBUG_VIRTIO_CONSUME_PATH
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
