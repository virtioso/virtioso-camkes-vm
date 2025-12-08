# Configuration Reference

This document provides a reference for all configuration options in the TII seL4 virtio system.

## CMake Configuration

### Platform Selection

```cmake
# Platform options
set(PLATFORM "rpi4" CACHE STRING "Target platform")
set(KernelPlatform "bcm2711" CACHE STRING "seL4 kernel platform")
set(KernelARMPlatform "bcm2711" CACHE STRING "ARM platform")
```

**Supported Platforms:**
| Platform | KernelPlatform | Description |
|----------|----------------|-------------|
| rpi4 | bcm2711 | Raspberry Pi 4 |
| qemu-arm-virt | qemu-arm-virt | QEMU ARM Virt |

### Build Type

```cmake
# Debug build
set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type")

# Release build
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type")
```

### seL4 Kernel Options

```cmake
# Kernel configuration
set(KernelVerificationBuild OFF CACHE BOOL "Verification build")
set(KernelDebugBuild ON CACHE BOOL "Debug build")
set(KernelPrinting ON CACHE BOOL "Enable kernel printing")
set(KernelBenchmarks "generic" CACHE STRING "Benchmark tracking")

# Virtualization
set(KernelArmHypervisorSupport ON CACHE BOOL "Enable hypervisor")
set(KernelArmVtimerUpdateVoffset ON CACHE BOOL "Update vtimer offset")

# SMP
set(KernelMaxNumNodes 4 CACHE STRING "Maximum CPU cores")
```

### VM Options

```cmake
# VM configuration
set(VmOnDemandDeviceInstall ON CACHE BOOL "On-demand device install")
set(VmPCISupport ON CACHE BOOL "Enable PCI support")
set(VmVirtioConsole ON CACHE BOOL "Enable virtio console")
set(VmInitRdFile "" CACHE FILEPATH "InitRD image file")
```

### TII-Specific Options

```cmake
# Large page support
set(LibSel4VmmmLargePages ON CACHE BOOL "Use large pages for guest RAM")

# PCIe/ECAM support
set(LibSel4VmmPCIeECAM ON CACHE BOOL "Enable ECAM for PCIe")

# Tracing
set(KernelBenchmarkTrackKernelEntries ON CACHE BOOL "Track kernel entries")
```

## CAmkES Configuration

### VM Component Definition

```camkes
component VM {
    VM_INIT_DEF()
    VM_TII_INIT_DEF()

    // Virtio device connections (for device VM)
    VIRTIO_COMPONENT_DEF(device_id, driver_id)
}
```

### VM Attributes

```camkes
configuration {
    // VM identification
    vm.vm_name = "my-vm";
    vm.vm_id = 0;

    // Memory configuration
    vm.ram_base = "0x40000000";
    vm.ram_size = "0x20000000";  // 512MB

    // CPU configuration
    vm._priority = 97;
    vm._affinity = 0;

    // Image loading
    vm.linux_image = "linux";
    vm.linux_address = "0x40080000";
    vm.dtb_address = "0x4f000000";
    vm.initrd_address = "0x4d000000";
}
```

### Virtio Device Attributes

```camkes
configuration {
    // Device VM virtio configuration
    vm0.vm_virtio_devices = [
        {
            id: 1,                           // Driver VM ID
            data_base: "0x50000000",         // Shared data base
            data_size: "0x10000000",         // Shared data size (256MB)
            ctrl_base: "0x60000000",         // Control region base
            ctrl_size: "0x1000"              // Control region size (4KB)
        }
    ];

    // Driver VM virtio configuration
    vm1.vm_virtio_drivers = [
        {
            id: 0,                           // Device VM ID
            data_base: "0x50000000",
            data_size: "0x10000000",
            ctrl_base: "0x60000000",
            ctrl_size: "0x1000"
        }
    ];
}
```

### Memory Configuration

```camkes
configuration {
    // Untyped memory pools
    vm.simple_untyped24_pool = 16;
    vm.simple_untyped20_pool = 4;
    vm.simple_untyped12_pool = 16;

    // Large page configuration
    vm.large_page_size = 21;  // 2MB pages
}
```

### IRQ Configuration

```camkes
configuration {
    // Virtual IRQ base
    vm.irq_base = 32;

    // Physical IRQ passthrough
    vm.irqs = [
        {
            irq: 148,        // Physical IRQ
            badge: 0x1       // Badge for notification
        }
    ];
}
```

## CAmkES Macros

### VM_INIT_DEF()

Standard VM initialization macro (from seL4 CAmkES VM):

```c
#define VM_INIT_DEF() \
    control; \
    uses FileServerInterface fs; \
    // ... standard VM interfaces
```

### VM_TII_INIT_DEF()

TII virtio extensions:

```c
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

Define virtio interfaces per VM pair:

```c
#define VIRTIO_COMPONENT_DEF(device_id, driver_id) \
    emits VirtIONotify virtio_vm##driver_id##_notify; \
    consumes VirtIONotify virtio_vm##driver_id##_downcall; \
    dataport Buf(4096) virtio_vm##driver_id##_iobuf; \
    dataport Buf virtio_vm##driver_id##_memdev;
```

### VIRTIO_COMPOSITION_DEF()

Connect device and driver VMs:

```c
#define VIRTIO_COMPOSITION_DEF(device_id, driver_id) \
    connection seL4SharedDataWithCaps virtio_##device_id##_##driver_id##_iobuf( \
        from vm##device_id.virtio_vm##driver_id##_iobuf, \
        to vm##driver_id##_virtio_vm##device_id.iobuf \
    ); \
    // ... additional connections
```

### VIRTIO_CONFIGURATION_DEF()

Set memory mapping configuration:

```c
#define VIRTIO_CONFIGURATION_DEF(device_id, driver_id, \
                                  data_base, data_size, \
                                  ctrl_base, ctrl_size) \
    vm##device_id.vm_virtio_devices = [ \
        { id: driver_id, ... } \
    ]; \
    vm##driver_id.vm_virtio_drivers = [ \
        { id: device_id, ... } \
    ];
```

## Platform Configuration Files

### Raspberry Pi 4

```cmake
# configs/raspberrypi4-64_defconfig
CONFIG_KernelPlatform=bcm2711
CONFIG_KernelARMPlatform=bcm2711
CONFIG_KernelArmHypervisorSupport=ON
CONFIG_KernelMaxNumNodes=4
```

### QEMU ARM Virt

```cmake
# configs/qemu-arm-virt_defconfig
CONFIG_KernelPlatform=qemu-arm-virt
CONFIG_KernelArmHypervisorSupport=ON
CONFIG_KernelMaxNumNodes=4
```

## Guest Linux Configuration

### Kernel Command Line

```bash
# Standard parameters
console=ttyAMA0
earlycon=pl011,0x9000000
root=/dev/vda2
rw

# SWIOTLB
swiotlb=65536  # 256MB

# Debug
loglevel=7
debug
```

### Device Tree Parameters

Parameters passed via generated FDT:

| Node | Property | Description |
|------|----------|-------------|
| memory | reg | RAM base and size |
| pci | ranges | PCI memory windows |
| pci | msi-parent | MSI controller phandle |
| reserved-memory | reg | SWIOTLB region |

## QEMU Configuration

### Device VM QEMU Options

```bash
qemu-system-aarch64 \
    -machine virt-sel4 \
    -accel sel4 \
    -m 512 \
    -nographic \
    -device virtio-blk-pci,drive=hd0 \
    -drive file=/var/lib/virt/images/user-vm.qcow2,id=hd0,format=qcow2 \
    -device virtio-net-pci,netdev=net0 \
    -netdev tap,id=net0,ifname=tap0,script=no \
    -device virtio-serial-pci \
    -chardev stdio,id=con0 \
    -device virtconsole,chardev=con0
```

### QEMU Machine Options

| Option | Value | Description |
|--------|-------|-------------|
| `-machine` | virt-sel4 | seL4-specific ARM Virt |
| `-accel` | sel4 | seL4 accelerator |
| `-m` | 512 | Memory size (MB) |

### QEMU Device Options

| Device | Options | Description |
|--------|---------|-------------|
| virtio-blk-pci | drive=X | Block storage |
| virtio-net-pci | netdev=X | Network |
| virtio-serial-pci | - | Serial console |
| virtio-9p-pci | fsdev=X | Filesystem sharing |

## Environment Variables

### Build Environment

```bash
# Cross compiler
export CROSS_COMPILE=aarch64-linux-gnu-

# Build directory
export BUILD_DIR=/path/to/build

# Yocto SSTATE
export SSTATE_DIR=/path/to/sstate-cache
```

### Runtime Environment

```bash
# QEMU paths
export QEMU_SYSTEM=/usr/bin/qemu-system-aarch64
export VM_IMAGES=/var/lib/virt/images
```

## Configuration Examples

### Minimal Two-VM Setup

```camkes
assembly {
    composition {
        component VM vm0;  // Device VM
        component VM vm1;  // Driver VM
        VIRTIO_COMPOSITION_DEF(0, 1)
    }

    configuration {
        // Device VM
        vm0.vm_name = "device-vm";
        vm0.ram_base = "0x40000000";
        vm0.ram_size = "0x20000000";
        vm0.vm_virtio_devices = [{
            id: 1,
            data_base: "0x50000000",
            data_size: "0x10000000",
            ctrl_base: "0x60000000",
            ctrl_size: "0x1000"
        }];

        // Driver VM
        vm1.vm_name = "driver-vm";
        vm1.ram_base = "0x80000000";
        vm1.ram_size = "0x20000000";
        vm1.vm_virtio_drivers = [{
            id: 0,
            data_base: "0x50000000",
            data_size: "0x10000000",
            ctrl_base: "0x60000000",
            ctrl_size: "0x1000"
        }];
    }
}
```

### Multi-User Configuration

```camkes
assembly {
    composition {
        component VM vm0;  // Device VM
        component VM vm1;  // Driver VM 1
        component VM vm2;  // Driver VM 2
        VIRTIO_COMPOSITION_DEF(0, 1)
        VIRTIO_COMPOSITION_DEF(0, 2)
    }

    configuration {
        vm0.vm_virtio_devices = [
            { id: 1, data_base: "0x50000000", ... },
            { id: 2, data_base: "0x70000000", ... }
        ];
        vm1.vm_virtio_drivers = [{ id: 0, ... }];
        vm2.vm_virtio_drivers = [{ id: 0, ... }];
    }
}
```

## Source Files

| File | Description |
|------|-------------|
| `configurations/tii/vm.h` | TII CAmkES macros |
| `configs/*.defconfig` | Platform configurations |
| `CMakeLists.txt` | CMake configuration |

## Related Documentation

- [Building](../getting-started/building.md)
- [CAmkES Templates](../components/camkes-templates.md)
- [Deployment Scenarios](../deployment/deployment-scenarios.md)
