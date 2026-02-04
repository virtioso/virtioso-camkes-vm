# Porting vm_qemu_virtio to Orin AGX

This document describes the port of the vm_qemu_virtio CAmkES application to NVIDIA Orin AGX (Tegra234).

## Architecture Overview

vm_qemu_virtio is a 2-VM virtio demo that runs two guest Linux VMs communicating via virtio:

```
┌──────────────────────────────────────────────────────────────┐
│                      seL4 Microkernel                        │
├──────────────────────────────────────────────────────────────┤
│  VM0 (Device VM)            │  VM1 (Driver VM)               │
│  ┌────────────────────┐     │  ┌────────────────────┐       │
│  │ QEMU + seL4 accel  │     │  │ virtio drivers     │       │
│  │ kmod-sel4-virt     │     │  │ User applications  │       │
│  │ Real HW: TCU, MGBE │     │  │ SWIOTLB bounce buf │       │
│  └─────────┬──────────┘     │  └─────────┬──────────┘       │
│            │                │            │                   │
│            └─────── RPC via shared dataport ─────────        │
│                    (iobuf + memdev)                          │
└──────────────────────────────────────────────────────────────┘
```

### VM Roles

| VM | Role | Description |
|----|------|-------------|
| VM0 | **Device VM** | Runs QEMU with seL4 accelerator. Gets all real hardware (TCU console, MGBE Ethernet, GPIO, HSP, BPMP). Provides virtio backends to VM1. |
| VM1 | **Driver VM** | Uses virtio devices from VM0. Only has PL011 emulation for early console. No real hardware passthrough. |

### Understanding the Naming Convention

The CAmkES attribute names now clearly describe the VM's role in each virtio channel:

| Attribute | VM Role | Meaning |
|-----------|---------|---------|
| `vm_virtio_device_channels` | Device | "I am the DEVICE in these channels" (provides virtio backends) |
| `vm_virtio_driver_channels` | Driver | "I am the DRIVER in these channels" (uses virtio devices) |

Usage is intuitive:
- VM0 (Device VM) has `vm0.vm_virtio_device_channels`
- VM1 (Driver VM) has `vm1.vm_virtio_driver_channels`

## Memory Layout

```
Physical Memory Map (Orin AGX vm_qemu_virtio):

0x80000000 - 0x8FFFFFFF : seL4 kernel + userspace
0x90000000 - 0xAFFFFFFF : VM0 (Device VM) RAM - 512MB
0xB0000000 - 0xBFFFFFFF : VM1 (Driver VM) RAM - 256MB
0xC0000000 - 0xC07FFFFF : Virtio Data (SWIOTLB bounce buffer) - 8MB
0xC0800000 - 0xC087FFFF : Virtio Control (RPC queues) - 512KB
```

### Key Addresses

| Symbol | Value | Purpose |
|--------|-------|---------|
| `VM0_RAM_BASE` | 0x90000000 | Device VM RAM start |
| `VM0_RAM_SIZE` | 0x20000000 | 512MB |
| `VM1_RAM_BASE` | 0xB0000000 | Driver VM RAM start |
| `VM1_RAM_SIZE` | 0x10000000 | 256MB |
| `VM0_VM1_VIRTIO_DATA_BASE` | 0xC0000000 | SWIOTLB bounce buffer |
| `VM0_VM1_VIRTIO_DATA_SIZE` | 0x800000 | 8MB |
| `VM0_VM1_VIRTIO_CTRL_BASE` | 0xC0800000 | RPC control queues |
| `VM0_VM1_VIRTIO_CTRL_SIZE` | 0x80000 | 512KB |
| `CONNECTION_BASE_ADDRESS` | 0x3F000000 | Cross-VM connection base (vPCI) |

## Device Passthrough

### VM0 (Device VM) - Full Hardware Access

VM0 gets all the real hardware needed for:
- **Console**: TCU (Tegra Combined UART) via HSP mailboxes
- **Networking**: MGBE0 10GbE Ethernet
- **Storage**: SDMMC4 internal eMMC
- **Clocks/Power**: BPMP via HSP Top0

DTB passthrough nodes:
```camkes
vm0.dtb = dtb([
    {\"path\": \"/bus@0/misc@100000\"},     /* APB MISC */
    {\"path\": \"/bus@0/gpio@2200000\"},    /* GPIO controller */
    {\"path\": \"/bus@0/pinmux@2430000\"},  /* Pinmux */
    {\"path\": \"/bus@0/memory-controller@2c00000\"}, /* MC for SMMU SID */
    {\"path\": \"/bus@0/serial@31d0000\"},  /* SBSA UART */
    {\"path\": \"/bus@0/mmc@3460000\"},     /* SDMMC4 eMMC */
    {\"path\": \"/bus@0/hsp@3c00000\"},     /* HSP Top0 - BPMP doorbell */
    {\"path\": \"/bus@0/phy@3f20000\"},     /* P2U PHY */
    {\"path\": \"/bus@0/ethernet@6800000\"},/* MGBE0 Ethernet */
    {\"path\": \"/bus@0/iommu@8000000\"},   /* smmu_niso1 (MGBE) */
    {\"path\": \"/bus@0/iommu@10000000\"},  /* smmu_iso */
    {\"path\": \"/bus@0/iommu@12000000\"},  /* smmu_niso0 */
    {\"path\": \"/bus@0/hsp@c150000\"},     /* HSP AON - TCU mailbox */
    {\"path\": \"/sram@40000000/sram@70000\"}, /* BPMP TX shmem */
    {\"path\": \"/sram@40000000/sram@71000\"}, /* BPMP RX shmem */
    {\"path\": \"/bpmp\"},                  /* BPMP node */
    {\"path\": \"/bus@0\"},                 /* bus parent */
]);
```

### VM1 (Driver VM) - Minimal Hardware

VM1 only needs:
- **Console**: PL011 emulation at 0x09000000 (VMM provides)
- **Reserved memory**: For memory reservation nodes

```camkes
vm1.dtb = dtb([
    {\"path\": \"/reserved-memory\"},
]);
vm1.pl011 = 0x9000000;  /* PL011 emulated by VMM */
```

## Console Configuration

### VM0 Console (TCU)

VM0 uses the real TCU console:
```
console=ttyTCU0 earlycon=pl011,mmio32,0x31d0000
```

The TCU node is generated programmatically by `fdt_plat_customize()` because it requires correct phandle references to HSP nodes.

### VM1 Console (PL011 Emulation)

VM1 uses VMM-emulated PL011:
```
console=hvc0 earlycon=pl011,mmio32,0x09000000
```

The `vm1.pl011 = 0x9000000` attribute enables PL011 emulation in the VMM. Output goes to seL4 serial.

## DTB Customization

The `fdt_plat_customize()` hook in `projects/vm/components/VM_Arm/src/modules/plat/orinagx/fdt.c` runs after CAmkES generates the base DTB.

For multi-VM support, it conditionally generates nodes based on device presence:

| Node | Required Devices | VM0 | VM1 |
|------|------------------|-----|-----|
| TCU serial | HSP Top0, HSP AON | Yes | No |
| PMC | GPIO | Yes | No |
| MGBE MAC | MGBE | Yes | No |

If required devices are missing, the generation is skipped (not an error).

## IRQ Routing

VM0 receives all hardware IRQs:

| Device | SPI | INTID |
|--------|-----|-------|
| GPIO banks | 288-335 | 320-367 |
| HSP Top0 doorbell | 176 | 208 |
| HSP AON shared1 | 133 | 165 |
| UARTI | 146 | 178 |
| SDMMC4 | 65 | 97 |
| P2U PHY | 203 | 235 |
| SMMU niso0 global | 170 | 202 |
| SMMU niso0 combined | 232 | 264 |
| SMMU niso1 global | 238 | 270 |
| SMMU niso1 combined | 242 | 274 |
| SMMU iso global | 240 | 272 |
| MGBE0 (6 IRQs) | 384-389 | 416-421 |

VM1 has no hardware IRQs - virtio IRQs are delivered via seL4 notifications.

## Virtio Configuration

The virtio connection is established by CAmkES macros:

```camkes
VIRTIO_CONFIGURATION_DEF(0, 1)  /* Device VM 0, Driver VM 1 */

vm0.vm_virtio_device_channels = [
    VIRTIO_CHANNEL_DEVICE_CONFIGURATION_DEF(0, 1)  /* VM0 is device side */
];

vm1.vm_virtio_driver_channels = [
    VIRTIO_CHANNEL_DRIVER_CONFIGURATION_DEF(0, 1)  /* VM1 is driver side */
];

VIRTIO_DRIVER_GUEST_RAM_CONFIGURATION_DEF(1)  /* VM1 RAM allocation */
```

## Files Modified

| File | Changes |
|------|---------|
| `apps/Arm/vm_qemu_virtio/settings.cmake` | Added orinagx to supported platforms |
| `templates/seL4VirtIODeviceVM.template.c` | Added CONNECTION_BASE_ADDRESS for orinagx, renamed to vm_virtio_device_channels |
| `templates/seL4VirtIODriverVM.template.c` | Renamed to vm_virtio_driver_channels |
| `configurations/tii/vm.h` | Renamed attributes and macros for clarity |
| `apps/Arm/vm_qemu_virtio/orinagx/devices.camkes` | Created - full platform configuration |
| `apps/Arm/vm_qemu_virtio/*/devices.camkes` | Updated to new terminology |
| `apps/Arm/vm_virtio_multi_user/*/devices.camkes` | Updated to new terminology |
| `projects/vm/components/VM_Arm/src/modules/plat/orinagx/fdt.c` | Made TCU/PMC/MAC generation conditional |

## Build and Test

### Build
```bash
cd ~/tii-sel4
make orinagx_defconfig
make vm_qemu_virtio
```

### Test
```bash
# Using autopilot MCP tools
mcp__sel4-autopilot__build_vm_minimal(mode="el2")
mcp__sel4-autopilot__test_vm_minimal(binary_path="...")
```

## Known Issues

1. **fdt_plat_customize()**: Must be conditional for multi-VM to avoid failures when devices aren't present. This has been implemented - node generation is skipped if required devices are missing.

## References

- `projects/vm-examples/apps/Arm/vm_minimal/orinagx/devices.camkes` - Single VM Orin AGX config
- `apps/Arm/vm_qemu_virtio/rpi4/devices.camkes` - RPi4 2-VM config (reference)
- `projects/tii-sel4-vm/docs/architecture/` - Virtio architecture docs
