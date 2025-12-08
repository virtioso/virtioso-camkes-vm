# Other Repository Changes

This document summarizes TII modifications across all seL4 ecosystem repositories beyond the main `tii-sel4-vm` project.

## Repository Overview

| Repository | Branch | Description |
|------------|--------|-------------|
| `kernel/` | rpi4 | seL4 microkernel |
| `projects/sel4_projects_libs/` | rpi4 | VMM libraries |
| `projects/vm/` | rpi4 | CAmkES VM framework |
| `projects/vm-linux/` | rpi4 | Guest Linux integration |
| `projects/vm-examples/` | rpi4 | Example applications |
| `tools/seL4/` | rpi4 | seL4 tools and elfloader |
| `tii_sel4_build/` | tii/development | Build system |
| `vm-images/meta-sel4/` | tii/development | Yocto layer |

## seL4 Projects Libraries (sel4_projects_libs)

### Large Page Mappings

**Purpose**: Enable 2MB page mappings for improved TLB efficiency.

**Key Changes**:

| File | Change |
|------|--------|
| `libsel4vm/src/guest_ram.c` | Large page detection and alignment |
| `libsel4vm/src/guest_memory.c` | Multiple RAM page size support |
| `libsel4vmmplatsupport/src/drivers/cross_vm_connection.c` | Dataport large pages |

**Configuration**:
```c
// Weak definition for large page support
bool guest_large_pages = false;

// In vm_ram_touch() - detect large page region
if (addr >= large_page_start && addr < large_page_end) {
    // Use 2MB alignment
}
```

### PCI/PCIe Enhancements

**Purpose**: Full PCIe support with ECAM (Extended Configuration Access Mechanism).

**Key Changes**:

| File | Change |
|------|--------|
| `libsel4vmmplatsupport/src/arch/arm/devices/vpci.c` | ECAM support, MSI parent |
| `libsel4vmmplatsupport/src/drivers/pci.c` | 32-bit config offsets |
| `libsel4vmmplatsupport/include/sel4vmmplatsupport/drivers/pci.h` | `vmm_pci_flags_t` enum |

**Features**:
- ECAM addressing (256 bytes → 4KB config space)
- MSI parent device tree property
- Non-existent device returns 0xFFFFFFFF per PCI spec
- Enlarged PCI memory window (512MB)

```c
// ECAM vs CAM addressing
typedef enum {
    PCI_BUS_CAM,    // Conventional (256 bytes)
    PCI_BUS_ECAM    // Extended (4KB)
} vmm_pci_flags_t;

// New functions
size_t vmm_pci_config_size(vmm_pci_flags_t flags);
bool vmm_pci_is_ecam(vmm_pci_address_t addr);
```

### IRQ Handling Improvements

**Purpose**: Proper level/edge triggered interrupt support.

**Key Changes**:

| File | Change |
|------|--------|
| `libsel4vm/src/arch/arm/vgic/vgic.c` | No duplicate IRQ queueing |
| `libsel4vm/src/arch/arm/vgic/vgic_v2.c` | Level IRQ support |
| `libsel4vm/src/arch/arm/vgic/vdist.h` | Trigger type support |

**New APIs**:
```c
// Set IRQ level (for level-triggered interrupts)
int vm_set_irq_level(vm_t *vm, int irq, int level);

// Get IRQ trigger type
int vm_vgic_irq_get_trigger(vm_t *vm, int irq);
```

### Memory Management

**Purpose**: RAM reservation API for lazy mapping.

**New APIs**:
```c
// Reserve RAM region for later mapping
int vm_ram_reserve(vm_t *vm, uintptr_t base, size_t size);

// Reserve at specific guest physical address
int vm_ram_reserve_at(vm_t *vm, uintptr_t gpa, size_t size);
```

## CAmkES VM Framework (projects/vm)

### VMM Enhancements

- Improved device handling
- Better fault management
- Platform-specific optimizations

## Guest Linux Integration (projects/vm-linux)

### Yocto Compatibility

**Changes**:
- Updated for Yocto compatibility
- Partition image support (vs raw disk images)
- SWIOTLB sizing configuration

### Device Tree Generation

**Changes**:
- Dynamic DTB generation for guest VMs
- Reserved memory node handling
- PCI node generation

### Key Files

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Build configuration |
| `vm-linux-helpers.cmake` | Rootfs overlay helpers |
| `linux-module-helpers.cmake` | Kernel module build |

## CAmkES VM Examples (projects/vm-examples)

### RPi4 Examples

**Added Examples**:
- `vm_minimal` - Basic VM configuration
- `vm_multi` - Multi-VM configuration
- PCI to VMM module integration

### Cross-VM Connector

**Location**: `apps/Arm/vm_cross_connector/`

**Components**:
- CAmkES specification
- Guest-side initialization
- Dataport/event handling

## seL4 Tools (tools/seL4)

### BCM2711 Elfloader

**Purpose**: Boot support for Raspberry Pi 4.

**Changes**:
- BCM2711 (RPi4 SoC) elfloader support
- ARM32 defaults for RPi4
- Platform detection

## Build System (tii_sel4_build)

### Docker Environment

**Location**: `docker/Dockerfile`

**Features**:
- Debian Bullseye base
- Cross-compiler (aarch64-linux-gnu)
- Haskell Stack for capdl
- Yocto dependencies

### Build Orchestration

**Location**: `Makefile`

**Targets**:
```makefile
make docker                    # Build container
make raspberrypi4-64_defconfig # Configure for RPi4
make linux-image               # Build Yocto images
make vm_qemu_virtio            # Build CAmkES app
```

### Configuration

**Location**: `configs/`

**Files**:
- `raspberrypi4-64_defconfig`
- `qemuarm64_defconfig`
- `raspberrypi4-64_trace_defconfig`

### CI/CD

**Location**: `.github/workflows/`

**Workflows**:
| Workflow | Purpose |
|----------|---------|
| `build-vm-images.yml` | Main build |
| `run_tests.yml` | Hardware testing |
| `pr.yml` | PR verification |

## Yocto Layer (vm-images/meta-sel4)

### Layer Structure

```
meta-sel4/
├── conf/
│   └── layer.conf
├── images/
│   ├── vm-image-driver.bb
│   ├── vm-image-user.bb
│   └── vm-image-boot.bb
├── recipes-kernel/
│   ├── kernel-module-sel4-virt/
│   └── cross-connector/
└── classes/
    └── vm-guest-image.bbclass
```

### Image Recipes

| Recipe | Description |
|--------|-------------|
| `vm-image-driver` | Device VM image (runs QEMU) |
| `vm-image-user` | Driver VM image (uses virtio) |
| `vm-image-boot` | Boot/initramfs image |

### Kernel Modules

| Module | Purpose |
|--------|---------|
| `kernel-module-sel4-virt` | virtio support |
| `kernel-module-sel4-tracebuffer` | Tracing |
| `cross-connector` | Cross-VM connection |

### Image Features

```bitbake
# vm-image-driver.bb
IMAGE_FEATURES += "qemu-virtualization benchmark"
```

## Commit Contributors

| Name | Email | Areas |
|------|-------|-------|
| Hannu Lyytinen | hannux@ssrc.tii.ae | Kernel, VMM, build |
| Markku Ahvenjärvi | markkux@ssrc.tii.ae | PCI, device tree |
| Ivan Kuznetsov | jsvapiav@gmail.com | RPi4, PCIe |
| Joonas Onatsu | joonasx@ssrc.tii.ae | Platform support |

## Upstream Compatibility

TII modifications maintain compatibility with upstream seL4:

- Changes are on separate branches (`rpi4`, `tii/development`)
- Core APIs preserved
- Additional features are opt-in via configuration

## Related Documentation

- [seL4 Kernel Modifications](kernel-modifications.md) - Kernel changes detail
- [Build Architecture](../build-system/build-architecture.md) - Build system
- [Yocto Integration](../build-system/yocto-integration.md) - Image generation
