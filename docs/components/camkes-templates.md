# CAmkES Templates

This document describes the CAmkES component templates for virtio device and driver VMs.

## Overview

CAmkES templates generate component code from specifications:

```mermaid
graph LR
    SPEC[".camkes Spec"] --> TEMPLATES["Templates"]
    TEMPLATES --> CODE["Generated C Code"]
    CODE --> BUILD["Build"]
    BUILD --> BINARY["Component Binary"]
```

## Template Files

| Template | Purpose |
|----------|---------|
| `seL4VirtIODeviceVM.template.c` | Device VM (runs QEMU) |
| `seL4VirtIODeviceVM.template.h` | Device VM header |
| `seL4VirtIODriverVM.template.c` | Driver VM (uses virtio) |
| `pl011.template.c` | PL011 UART driver |

## Configuration Macros

### VM_TII_INIT_DEF()

Adds virtio device/driver attributes to VM components:

```c
// From configurations/tii/vm.h
#define VM_TII_INIT_DEF() \
    attribute { \
        int id; \
        string data_base; \
        string data_size; \
        string ctrl_base; \
        string ctrl_size; \
    } vm_virtio_devices[]; \
    attribute { \
        int id; \
        string data_base; \
        string data_size; \
        string ctrl_base; \
        string ctrl_size; \
    } vm_virtio_drivers[];
```

### VIRTIO_COMPONENT_DEF()

Defines virtio communication interfaces per VM pair:

```c
#define VIRTIO_COMPONENT_DEF(device_id, driver_id) \
    /* Notifications */ \
    emits VirtIONotify virtio_vm##driver_id##_notify; \
    consumes VirtIONotify virtio_vm##driver_id##_downcall; \
    /* Dataports */ \
    dataport Buf(4096) virtio_vm##driver_id##_iobuf; \
    dataport Buf virtio_vm##driver_id##_memdev;
```

### VIRTIO_COMPOSITION_DEF()

Connects device and driver VMs:

```c
#define VIRTIO_COMPOSITION_DEF(device_id, driver_id) \
    /* I/O buffer connection */ \
    connection seL4SharedDataWithCaps virtio_##device_id##_##driver_id##_iobuf( \
        from vm##device_id.virtio_vm##driver_id##_iobuf, \
        to vm##driver_id##_virtio_vm##device_id.iobuf \
    ); \
    /* Memory device connection */ \
    connection seL4SharedDataWithCaps virtio_##device_id##_##driver_id##_memdev( \
        from vm##device_id.virtio_vm##driver_id##_memdev, \
        to vm##driver_id##_virtio_vm##device_id.memdev \
    ); \
    /* Notifications */ \
    connection seL4GlobalAsynch virtio_##device_id##_##driver_id##_notify( \
        from vm##device_id.virtio_vm##driver_id##_notify, \
        to vm##driver_id##_virtio_vm##device_id.notify \
    ); \
    connection seL4Notification virtio_##device_id##_##driver_id##_downcall( \
        from vm##driver_id##_virtio_vm##device_id.downcall, \
        to vm##device_id.virtio_vm##driver_id##_downcall \
    );
```

### VIRTIO_CONFIGURATION_DEF()

Sets memory mapping configuration:

```c
#define VIRTIO_CONFIGURATION_DEF(device_id, driver_id, \
                                  data_base, data_size, \
                                  ctrl_base, ctrl_size) \
    vm##device_id.vm_virtio_devices = [ \
        { \
            id: driver_id, \
            data_base: data_base, \
            data_size: data_size, \
            ctrl_base: ctrl_base, \
            ctrl_size: ctrl_size \
        } \
    ]; \
    vm##driver_id.vm_virtio_drivers = [ \
        { \
            id: device_id, \
            data_base: data_base, \
            data_size: data_size, \
            ctrl_base: ctrl_base, \
            ctrl_size: ctrl_size \
        } \
    ];
```

## Device VM Template

### seL4VirtIODeviceVM.template.c

Generated code for device VM (runs QEMU):

```c
// Template variables
/*- set vm_id = me.name -*/
/*- set drivers = configuration[me.name].get('vm_virtio_devices', []) -*/

#include <sel4/sel4.h>
#include <camkes.h>

// Per-driver VM connections
/*- for driver in drivers -*/
static io_proxy_t io_proxy_/*? driver.id ?*/;

void /*? me.name ?*/_virtio_vm/*? driver.id ?*/_init(void) {
    // Initialize I/O proxy for driver VM /*? driver.id ?*/
    io_proxy_init(
        &io_proxy_/*? driver.id ?*/,
        /*? driver.data_base ?*/,
        /*? driver.data_size ?*/,
        /*? driver.ctrl_base ?*/,
        /*? driver.ctrl_size ?*/,
        virtio_vm/*? driver.id ?*/_iobuf,
        virtio_vm/*? driver.id ?*/_memdev,
        virtio_vm/*? driver.id ?*/_notify_emit,
        virtio_vm/*? driver.id ?*/_downcall_wait
    );
}
/*- endfor -*/

// Main VM thread
int run(void) {
    // Initialize all driver connections
    /*- for driver in drivers -*/
    /*? me.name ?*/_virtio_vm/*? driver.id ?*/_init();
    /*- endfor -*/

    // Start VM
    return vm_run();
}
```

### seL4VirtIODeviceVM.template.h

Header with declarations:

```c
/*- set drivers = configuration[me.name].get('vm_virtio_devices', []) -*/

#pragma once

#include <tii/io_proxy.h>

/*- for driver in drivers -*/
extern io_proxy_t io_proxy_/*? driver.id ?*/;
void /*? me.name ?*/_virtio_vm/*? driver.id ?*/_init(void);
/*- endfor -*/
```

## Driver VM Template

### seL4VirtIODriverVM.template.c

Generated code for driver VM (uses virtio):

```c
/*- set vm_id = me.name -*/
/*- set devices = configuration[me.name].get('vm_virtio_drivers', []) -*/

#include <sel4/sel4.h>
#include <camkes.h>
#include <tii/vmm.h>

// Per-device VM connections
/*- for device in devices -*/
static virtio_connection_t virtio_conn_/*? device.id ?*/;
/*- endfor -*/

static void init_virtio_connections(vm_t *vm) {
    /*- for device in devices -*/
    virtio_connection_init(
        &virtio_conn_/*? device.id ?*/,
        vm,
        /*? device.data_base ?*/,
        /*? device.data_size ?*/,
        /*? device.ctrl_base ?*/,
        /*? device.ctrl_size ?*/,
        virtio_vm/*? device.id ?*/_iobuf,
        virtio_vm/*? device.id ?*/_memdev
    );
    /*- endfor -*/
}

int run(void) {
    vm_t vm;
    vm_init(&vm);

    init_virtio_connections(&vm);

    return vm_run(&vm);
}
```

## Example CAmkES Specification

### Two-VM Configuration

```camkes
// vm_qemu_virtio.camkes
import <VM/vm.camkes>;
import "tii/vm.h";

component VM0 {
    VM_INIT_DEF()
    VM_TII_INIT_DEF()
    VIRTIO_COMPONENT_DEF(0, 1)
}

component VM1 {
    VM_INIT_DEF()
    VM_TII_INIT_DEF()
}

assembly {
    composition {
        component VM0 vm0;
        component VM1 vm1;

        VIRTIO_COMPOSITION_DEF(0, 1)
    }

    configuration {
        // Device VM (VM0) configuration
        vm0.vm_name = "device-vm";
        vm0.vm_virtio_devices = [
            {
                id: 1,
                data_base: "0x50000000",
                data_size: "0x10000000",
                ctrl_base: "0x60000000",
                ctrl_size: "0x1000"
            }
        ];

        // Driver VM (VM1) configuration
        vm1.vm_name = "driver-vm";
        vm1.vm_virtio_drivers = [
            {
                id: 0,
                data_base: "0x50000000",
                data_size: "0x10000000",
                ctrl_base: "0x60000000",
                ctrl_size: "0x1000"
            }
        ];
    }
}
```

### Three-VM Configuration

```camkes
// vm_virtio_multi_user.camkes
assembly {
    composition {
        component VM0 vm0;  // Device VM
        component VM1 vm1;  // Driver VM 1
        component VM2 vm2;  // Driver VM 2

        VIRTIO_COMPOSITION_DEF(0, 1)
        VIRTIO_COMPOSITION_DEF(0, 2)
    }

    configuration {
        vm0.vm_virtio_devices = [
            { id: 1, ... },
            { id: 2, ... }
        ];
        vm1.vm_virtio_drivers = [{ id: 0, ... }];
        vm2.vm_virtio_drivers = [{ id: 0, ... }];
    }
}
```

## Adding New Device Types

### Step 1: Define Component

```camkes
component MyDevice {
    // Standard VM init
    VM_INIT_DEF()
    VM_TII_INIT_DEF()

    // Custom interfaces
    provides MyDeviceInterface api;
}
```

### Step 2: Create Template

```c
// my_device.template.c
/*- set config = configuration[me.name] -*/

#include <camkes.h>

void my_device_init(void) {
    // Device-specific initialization
}

int run(void) {
    my_device_init();
    return 0;
}
```

### Step 3: Register Template

```cmake
# CMakeLists.txt
CAmkESAddTemplatesPath(${CMAKE_CURRENT_LIST_DIR}/templates)
```

## Template Variables

| Variable | Description |
|----------|-------------|
| `me.name` | Component instance name |
| `configuration[me.name]` | Component configuration |
| `me.interface` | Interface being generated |

## CMake Integration

### Template Path Registration

```cmake
# From tii_camkes_vm_helpers.cmake
macro(DeclareTIICAmkESVM N)
    CAmkESAddTemplatesPath(
        ${TII_CAMKES_VM_DIR}/templates
    )

    # Add VM component
    DeclareCAmkESVM(${N})
endmacro()
```

## Source Files

| File | Description |
|------|-------------|
| `templates/seL4VirtIODeviceVM.template.c` | Device VM template |
| `templates/seL4VirtIODeviceVM.template.h` | Device VM header |
| `templates/seL4VirtIODriverVM.template.c` | Driver VM template |
| `configurations/tii/vm.h` | Configuration macros |

## Related Documentation

- [Virtio Architecture](../architecture/virtio-architecture.md)
- [I/O Proxy](io-proxy.md)
- [Building](../getting-started/building.md)
