# Porting vm-image-minimal to NVIDIA Orin AGX

This document describes the process of porting the `vm_minimal` CAmkES application and its associated Yocto guest image to the NVIDIA Orin AGX (Tegra234) platform.

## Table of Contents

1. [Overview](#overview)
2. [Platform Background](#platform-background)
3. [Yocto Layer Integration](#yocto-layer-integration)
4. [Machine Configuration](#machine-configuration)
5. [Kernel Configuration](#kernel-configuration)
6. [CAmkES Application Support](#camkes-application-support)
7. [Device Tree Extensions](#device-tree-extensions)
8. [Build Instructions](#build-instructions)
9. [Known Issues and Limitations](#known-issues-and-limitations)

---

## Overview

The goal is to run a minimal Linux guest VM under the seL4 hypervisor on Orin AGX. This requires:

1. **Yocto integration**: Build guest Linux images using meta-tegra layer
2. **CAmkES configuration**: Define VM memory layout and device passthrough
3. **Device tree**: Extend seL4's DTS with devices needed by the guest

### Architecture

```
┌─────────────────────────────────────────────────┐
│              seL4 Microkernel (EL2)             │
├─────────────────────────────────────────────────┤
│                   VMM Component                  │
├─────────────────────────────────────────────────┤
│              Guest Linux VM (EL1)               │
│  ┌───────────────────────────────────────────┐  │
│  │  L4T Kernel 5.15 with:                    │  │
│  │  - BPMP driver (clocks, resets, power)    │  │
│  │  - Virtio drivers (net, block, console)   │  │
│  │  - TCU serial console                     │  │
│  └───────────────────────────────────────────┘  │
└─────────────────────────────────────────────────┘
```

---

## Platform Background

### NVIDIA Orin AGX (Tegra234)

- **CPU**: 12x ARM Cortex-A78 cores (3 clusters of 4)
- **Memory**: Up to 64GB LPDDR5
- **SoC**: Tegra234 with BPMP co-processor

### Key Hardware Components

| Component | Purpose | Address |
|-----------|---------|---------|
| BPMP | Boot and Power Management Processor - controls clocks, resets, power domains | Virtual (IPC-based) |
| HSP Top0 | Hardware Synchronization Primitives - mailbox for BPMP communication | 0x03c00000 |
| HSP AON | HSP for Always-On domain - used by TCU | 0x0c150000 |
| SRAM | Shared memory for CPU-BPMP communication | 0x40000000 |
| TCU | Tegra Combined UART - debug console via HSP | 0x0c168000 |

### Why BPMP Matters

Unlike simpler platforms (RPi4, QEMU), Tegra SoCs require the BPMP co-processor for:

- **Clock control**: All peripheral clocks are managed by BPMP
- **Reset control**: Device resets go through BPMP
- **Power domains**: Enabling/disabling power to subsystems

Without BPMP access, the guest Linux cannot:
- Initialize most peripherals (they'd have no clocks)
- Perform proper power management
- Use many Tegra-specific drivers

---

## Yocto Layer Integration

### Rationale

We use [OE4T/meta-tegra](https://github.com/OE4T/meta-tegra) because:

1. **L4T kernel**: Provides NVIDIA's Linux for Tegra kernel with all Tegra-specific drivers
2. **BPMP driver**: The L4T kernel includes the tegra-bpmp driver for clock/reset/power
3. **Community maintained**: Active OpenEmbedded layer compatible with Yocto Scarthgap

### Files Created

#### 1. Clone meta-tegra layer

```bash
cd vm-images/
git clone -b scarthgap https://github.com/OE4T/meta-tegra.git
```

The `scarthgap` branch matches our Yocto release (both use scarthgap).

#### 2. Update setup.sh

**File**: `tii_sel4_build/yocto/setup.sh`

Added meta-tegra to the layer configuration:

```bash
grep meta-tegra conf/bblayers.conf 2>/dev/null 1>&2 || \
  printf 'BBLAYERS += "%s/meta-tegra"\n' "$LAYERS_ROOT" >> conf/bblayers.conf
```

**Rationale**: This automatically adds meta-tegra to `bblayers.conf` when sourcing the setup script, following the same pattern as meta-raspberrypi.

#### 3. Update layer.conf for dynamic-layers

**File**: `vm-images/meta-sel4/conf/layer.conf`

Added tegra to BBFILES_DYNAMIC:

```bitbake
BBFILES_DYNAMIC += " \
    raspberrypi:${LAYERDIR}/dynamic-layers/raspberrypi/*/*/*.bb \
    raspberrypi:${LAYERDIR}/dynamic-layers/raspberrypi/*/*/*.bbappend \
    tegra:${LAYERDIR}/dynamic-layers/tegra/*/*/*.bb \
    tegra:${LAYERDIR}/dynamic-layers/tegra/*/*/*.bbappend \
"
```

**Rationale**: Dynamic layers allow meta-sel4 to provide tegra-specific customizations only when meta-tegra is present, without hard dependencies.

#### 4. Create tegra kernel bbappend

**File**: `vm-images/meta-sel4/dynamic-layers/tegra/recipes-kernel/linux/linux-jammy-nvidia-tegra_%.bbappend`

```bitbake
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

# Include virtio driver support for seL4 VM guests
include recipes-kernel/linux/linux-virtio.inc
```

**Rationale**: The L4T kernel doesn't enable virtio drivers by default. This bbappend includes our standard virtio configuration fragment to enable:
- `CONFIG_VIRTIO`
- `CONFIG_VIRTIO_PCI`
- `CONFIG_VIRTIO_NET`
- `CONFIG_VIRTIO_BLK`
- `CONFIG_VIRTIO_CONSOLE`

---

## Machine Configuration

**File**: `vm-images/meta-sel4/meta-sel4/conf/machine/vm-jetson-agx-orin.conf`

```bitbake
# VM machine configuration for Jetson AGX Orin
# Used for building guest VM images to run under seL4 VMM

# Inherit from Tegra machine for L4T kernel with BPMP driver
MACHINEOVERRIDES = "tegra:tegra234:jetson-agx-orin-devkit:${MACHINE}"

# Common seL4 VM settings (serial console)
require include/sel4-vm.inc

# Base Tegra machine config - provides L4T kernel with BPMP support
require conf/machine/jetson-agx-orin-devkit.conf

# Override serial console for VM guest (virtio console or standard serial)
SERIAL_CONSOLES = "115200;ttyAMA0 115200;hvc0"

# We don't need Tegra-specific boot/flash machinery for VM guests
# The guest kernel is loaded by seL4 VMM, not via UEFI/bootloader
UBOOT_MACHINE = ""
IMAGE_CLASSES:remove = "image_types_tegra"

# Prefer simpler image types for VM guests
IMAGE_FSTYPES = "ext4 tar.bz2 cpio.gz"
```

### Design Decisions

| Setting | Value | Rationale |
|---------|-------|-----------|
| `MACHINEOVERRIDES` | tegra:tegra234:... | Inherit Tegra234-specific recipes and configurations |
| `require jetson-agx-orin-devkit.conf` | Base config | Get L4T kernel, BPMP support, Tegra BSP |
| `SERIAL_CONSOLES` | ttyAMA0, hvc0 | VM uses PL011 or virtio-console, not Tegra UART |
| `UBOOT_MACHINE = ""` | Empty | Guest doesn't need U-Boot; seL4 VMM loads kernel directly |
| `IMAGE_CLASSES:remove` | image_types_tegra | Don't generate Tegra flash images (not needed for VM) |
| `IMAGE_FSTYPES` | ext4, tar.bz2, cpio.gz | Simple formats for VMM consumption |

### Why Not Use Stock jetson-agx-orin-devkit?

The stock machine config is designed for bare-metal boot and includes:
- Tegra-specific image types (tegraflash, etc.)
- U-Boot configuration
- Flash partition layouts

For VM guests, we only need:
- The kernel with BPMP driver
- A root filesystem (cpio.gz for initramfs)
- Virtio driver support

---

## Kernel Configuration

### L4T Kernel vs Mainline

We use the L4T (Linux for Tegra) kernel because:

| Feature | L4T Kernel | Mainline Kernel |
|---------|------------|-----------------|
| BPMP driver | Full support | Limited/missing |
| Tegra clocks | Complete | Incomplete |
| Power domains | Yes | Partial |
| Tegra-specific HW | Supported | Often missing |

### Virtio Support

The `linux-virtio.inc` configuration fragment enables:

```
CONFIG_VIRTIO=y
CONFIG_VIRTIO_PCI=y
CONFIG_VIRTIO_MMIO=y
CONFIG_VIRTIO_NET=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_HVC_DRIVER=y
```

This allows the guest to use virtio devices provided by the seL4 VMM or QEMU backend.

---

## CAmkES Application Support

### Files Modified

#### 1. settings.cmake

**File**: `projects/vm-examples/apps/Arm/vm_minimal/settings.cmake`

Added orinagx to supported platforms:

```cmake
set(supported "tk1;tx1;tx2;exynos5422;qemu-arm-virt;odroidc2;rpi4;orinagx")
```

Added orinagx configuration:

```cmake
if(${PLATFORM} STREQUAL "orinagx")
    set(VmPCISupport ON CACHE BOOL "" FORCE)
    set(VmVirtioNet ON CACHE BOOL "" FORCE)
    set(VmInitRdFile ON CACHE BOOL "" FORCE)
    set(VmDtbFile ON CACHE BOOL "" FORCE)
endif()
```

| Setting | Purpose |
|---------|---------|
| `VmPCISupport` | Enable PCI bus emulation for virtio-pci devices |
| `VmVirtioNet` | Enable virtio network device support |
| `VmInitRdFile` | Load initramfs from file server |
| `VmDtbFile` | Load device tree from file server |

#### 2. CMakeLists.txt

**File**: `projects/vm-examples/apps/Arm/vm_minimal/CMakeLists.txt`

Added orinagx build case:

```cmake
elseif("${KernelARMPlatform}" STREQUAL "orinagx")
    find_package(camkes-vm-linux REQUIRED)
    include(${CAMKES_VM_LINUX_HELPERS_PATH})
    set(cpp_flags "-DKERNELARMPLATFORM_ORINAGX")

    AddToFileServer("linux" "${CAMKES_VM_IMAGES_DIR}/orinagx/linux")
    set(rootfs_file "${CAMKES_VM_IMAGES_DIR}/orinagx/rootfs.cpio.gz")
    AddToFileServer("linux-initrd" ${rootfs_file})
    UpdateDtbFromInitrd(
        "${CAMKES_VM_IMAGES_DIR}/orinagx/linux-dtb"
        ${rootfs_file}
        "0x9D000000"
        dtb_gen_target
        output_dtb_location
    )
    AddToFileServer("linux-dtb" "${output_dtb_location}" DEPENDS dtb_gen_target)
endif()
```

#### 3. devices.camkes

**File**: `projects/vm-examples/apps/Arm/vm_minimal/orinagx/devices.camkes`

```c
#include <configurations/vm.h>

#define VM_INITRD_MAX_SIZE 0x2000000  /* 32 MB */
#define VM_RAM_BASE     0x90000000
#define VM_RAM_SIZE     0x10000000   /* 256 MB */
#define VM_RAM_OFFSET   0
#define VM_DTB_ADDR     0x9E000000
#define VM_INITRD_ADDR  0x9D000000

assembly {
    composition {}
    configuration {
        vm0.linux_address_config = {
            "linux_ram_base" : VAR_STRINGIZE(VM_RAM_BASE),
            "linux_ram_paddr_base" : VAR_STRINGIZE(VM_RAM_BASE),
            "linux_ram_size" : VAR_STRINGIZE(VM_RAM_SIZE),
            "linux_ram_offset" : VAR_STRINGIZE(VM_RAM_OFFSET),
            "dtb_addr" : VAR_STRINGIZE(VM_DTB_ADDR),
            "initrd_max_size" : VAR_STRINGIZE(VM_INITRD_MAX_SIZE),
            "initrd_addr" : VAR_STRINGIZE(VM_INITRD_ADDR)
        };

        vm0.linux_image_config = {
            "linux_bootcmdline" : "console=ttyTCU0,115200 earlycon=tegra_comb_uart,mmio32,0x0c168000 debug init=/sbin/init",
            "linux_stdout" : "/serial@c168000",
        };
        vm0.num_vcpus = 4;

        vm0.dtb = dtb([
                       {"path": "/serial@c168000"},
                       {"path": "/tegra-hsp@3c00000"},
                       {"path": "/tegra-hsp@c150000"},
                       {"path": "/sram@40000000"},
                       {"path": "/bpmp"}]);

        vm0.untyped_mmios = [
            "0x90000000:28",   /* 256MB at VM_RAM_BASE */
        ];
    }
}
```

### Memory Layout Rationale

| Region | Address | Size | Purpose |
|--------|---------|------|---------|
| seL4 Kernel | 0x80400000+ | ~64MB | Kernel, root task, initial objects |
| VM RAM | 0x90000000 | 256MB | Guest Linux memory |
| DTB | 0x9E000000 | ~1MB | Device tree blob |
| Initrd | 0x9D000000 | 32MB max | Initial ramdisk |

The VM RAM is placed at 0x90000000 to:
- Avoid conflict with seL4 kernel at 0x80400000
- Stay within 32-bit addressable range for compatibility
- Leave room for growth

### Device Passthrough

The guest needs these devices for BPMP communication:

| Device | Path | Purpose |
|--------|------|---------|
| TCU | `/serial@c168000` | Debug console (uses HSP mailbox) |
| HSP Top0 | `/tegra-hsp@3c00000` | BPMP doorbell mailbox |
| HSP AON | `/tegra-hsp@c150000` | TCU communication |
| SRAM | `/sram@40000000` | CPU-BPMP shared memory buffers |
| BPMP | `/bpmp` | Clock/reset/power control |

---

## Device Tree Extensions

**File**: `kernel/tools/dts/orinagx.dts`

The seL4 kernel's minimal DTS was extended with BPMP-related devices:

### HSP Top0 (Hardware Synchronization Primitives)

```dts
hsp_top0: tegra-hsp@3c00000 {
    compatible = "nvidia,tegra234-hsp", "nvidia,tegra194-hsp";
    reg = <0x0 0x03c00000 0x0 0xa0000>;
    interrupts = <0x0 176 0x4>,   /* doorbell */
                 <0x0 120 0x4>,   /* shared0 */
                 /* ... shared1-7 ... */
                 <0x0 127 0x4>;   /* shared7 */
    interrupt-names = "doorbell", "shared0", /* ... */ "shared7";
    #mbox-cells = <2>;
    status = "okay";
};
```

**Purpose**: Provides the doorbell mechanism for CPU to signal BPMP.

### HSP AON

```dts
hsp_aon: tegra-hsp@c150000 {
    compatible = "nvidia,tegra234-hsp", "nvidia,tegra194-hsp";
    reg = <0x0 0x0c150000 0x0 0x90000>;
    interrupts = <0x0 133 0x4>, <0x0 134 0x4>,
                 <0x0 135 0x4>, <0x0 136 0x4>;
    interrupt-names = "shared1", "shared2", "shared3", "shared4";
    #mbox-cells = <2>;
    status = "okay";
};
```

**Purpose**: Used by TCU for console I/O.

### SRAM (Shared Memory)

```dts
sram@40000000 {
    compatible = "nvidia,tegra234-sysram", "mmio-sram";
    reg = <0x0 0x40000000 0x0 0x80000>;
    #address-cells = <1>;
    #size-cells = <1>;
    ranges = <0x0 0x0 0x40000000 0x80000>;
    no-memory-wc;

    cpu_bpmp_tx: sram@70000 {
        reg = <0x70000 0x1000>;
        label = "cpu-bpmp-tx";
        pool;
    };

    cpu_bpmp_rx: sram@71000 {
        reg = <0x71000 0x1000>;
        label = "cpu-bpmp-rx";
        pool;
    };
};
```

**Purpose**: Provides the shared memory buffers for CPU-BPMP message passing:
- `cpu_bpmp_tx`: CPU writes requests here
- `cpu_bpmp_rx`: BPMP writes responses here

### BPMP Node

```dts
bpmp: bpmp {
    compatible = "nvidia,tegra234-bpmp", "nvidia,tegra186-bpmp";
    mboxes = <&hsp_top0 0x2 0x13>;  /* DB type, BPMP master */
    shmem = <&cpu_bpmp_tx>, <&cpu_bpmp_rx>;
    #clock-cells = <1>;
    #reset-cells = <1>;
    #power-domain-cells = <1>;
    status = "okay";

    bpmp_i2c: i2c {
        compatible = "nvidia,tegra186-bpmp-i2c";
        nvidia,bpmp-bus-id = <5>;
        #address-cells = <1>;
        #size-cells = <0>;
    };
};
```

**Purpose**: The main BPMP driver node that:
- References HSP for doorbell signaling
- References SRAM for message buffers
- Provides clock, reset, and power-domain provider interfaces

---

## Build Instructions

### Prerequisites

1. Docker container built: `make docker`
2. meta-tegra cloned to `vm-images/meta-tegra`

### Build Guest Image

```bash
# Enter container
make shell

# Setup Yocto environment for Orin AGX VM
cd vm-images
MACHINE=vm-jetson-agx-orin source setup.sh

# Build minimal image
bitbake vm-image-minimal
```

Output: `vm-images/build/tmp/deploy/images/vm-jetson-agx-orin/`
- `Image` - Linux kernel
- `vm-image-minimal-vm-jetson-agx-orin.cpio.gz` - Root filesystem

### Build CAmkES Application

```bash
# Configure for Orin AGX
# (Requires orinagx_defconfig in tii_sel4_build/Makefile)

# Build vm_minimal
make vm_minimal
```

### Deploy

Copy the built images to the appropriate location:
```bash
mkdir -p projects/camkes-vm-images/orinagx/
cp vm-images/build/tmp/deploy/images/vm-jetson-agx-orin/Image \
   projects/camkes-vm-images/orinagx/linux
cp vm-images/build/tmp/deploy/images/vm-jetson-agx-orin/vm-image-minimal-*.cpio.gz \
   projects/camkes-vm-images/orinagx/rootfs.cpio.gz
```

---

## Known Issues and Limitations

### 1. BPMP Firmware Compatibility

The BPMP co-processor runs its own firmware that must be compatible with the L4T kernel's BPMP driver. The firmware is loaded by earlier boot stages (MB1/MB2) before seL4 starts.

**Impact**: Cannot upgrade BPMP driver independently of firmware.

### 2. Interrupt Virtualization

The GICv3 interrupt controller requires proper virtualization support. Some HSP interrupts may need to be:
- Trapped and emulated by VMM
- Or directly injected to guest

**Status**: Needs testing to verify interrupt delivery to guest.

### 3. Memory-Mapped Device Access

The guest needs direct access to:
- HSP registers (0x03c00000, 0x0c150000)
- SRAM (0x40000000)

**Requirement**: VMM must map these regions into guest's stage-2 page tables.

### 4. Console Output

The TCU console works via HSP mailbox, not a traditional UART. The guest kernel needs:
- `earlycon=tegra_comb_uart,mmio32,0x0c168000` for early boot messages
- `console=ttyTCU0` for runtime console

### 5. Clock/Reset During Boot

Some clocks may need to be enabled before the guest's BPMP driver initializes. This depends on what seL4/elfloader has configured.

---

## References

- [OE4T/meta-tegra](https://github.com/OE4T/meta-tegra) - Yocto layer for Tegra
- [NVIDIA L4T Documentation](https://docs.nvidia.com/jetson/) - Jetson Linux documentation
- [Tegra234 TRM](https://developer.nvidia.com/embedded/downloads) - Technical Reference Manual
- [seL4 ARM VMM](https://docs.sel4.systems/projects/camkes-vm/) - CAmkES VM documentation
