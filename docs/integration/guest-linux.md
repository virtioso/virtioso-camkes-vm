# Guest Linux Configuration

This document describes the Linux kernel configuration and setup required for guest VMs.

## Overview

Guest Linux VMs require specific configuration for:

- Device tree compatibility with VMM-generated FDT
- SWIOTLB for DMA bounce buffering
- virtio-pci drivers for virtio devices
- UIO framework for cross-VM communication
- QEMU support (device VM only)

## Kernel Configuration

### Required Options

```kconfig
# Virtio support
CONFIG_VIRTIO=y
CONFIG_VIRTIO_PCI=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_NET=y
CONFIG_VIRTIO_CONSOLE=y

# PCI support
CONFIG_PCI=y
CONFIG_PCI_HOST_GENERIC=y
CONFIG_PCIEPORTBUS=y

# DMA and SWIOTLB
CONFIG_SWIOTLB=y
CONFIG_DMA_RESTRICTED_POOL=y

# UIO for cross-VM connections
CONFIG_UIO=y
CONFIG_UIO_PCI_GENERIC=y

# ARM GIC
CONFIG_ARM_GIC=y
CONFIG_ARM_GIC_V2M=y
```

### Device VM Additional Options

```kconfig
# KVM (for nested guests if needed)
CONFIG_KVM=y
CONFIG_KVM_ARM_HOST=y

# QEMU requirements
CONFIG_VHOST=y
CONFIG_VHOST_NET=y
```

### Recommended Options

```kconfig
# Debugging
CONFIG_DEBUG_INFO=y
CONFIG_EARLY_PRINTK=y
CONFIG_PRINTK_TIME=y

# Performance
CONFIG_NO_HZ_IDLE=y
CONFIG_HIGH_RES_TIMERS=y
CONFIG_PREEMPT_VOLUNTARY=y
```

## Device Tree Requirements

### Memory Node

The VMM generates a memory node describing available RAM:

```dts
memory@40000000 {
    device_type = "memory";
    reg = <0x0 0x40000000 0x0 0x20000000>;  /* 512MB at 0x40000000 */
};
```

Guest kernel boot parameter:
```
mem=512M
```

### PCI Host Bridge

```dts
pci@70000000 {
    compatible = "pci-host-ecam-generic";
    device_type = "pci";
    #address-cells = <3>;
    #size-cells = <2>;
    #interrupt-cells = <1>;

    /* ECAM config space */
    reg = <0x0 0x70000000 0x0 0x1000000>;

    /* Bus range */
    bus-range = <0x0 0x0>;

    /* Memory ranges */
    ranges = <0x2000000 0x0 0x50000000
              0x0 0x50000000
              0x0 0x20000000>;  /* 512MB PCI memory */

    /* MSI controller */
    msi-parent = <&gicv2m>;

    /* Interrupt mapping */
    interrupt-map = <...>;
    interrupt-map-mask = <0x1800 0x0 0x0 0x7>;
};
```

### GICv2m for MSI

```dts
gic: interrupt-controller@8000000 {
    compatible = "arm,gic-400";
    #interrupt-cells = <3>;
    interrupt-controller;
    reg = <0x0 0x8000000 0x0 0x10000>,   /* Distributor */
          <0x0 0x8010000 0x0 0x10000>;   /* CPU interface */

    gicv2m: v2m@8020000 {
        compatible = "arm,gic-v2m-frame";
        msi-controller;
        reg = <0x0 0x8020000 0x0 0x1000>;
        arm,msi-base-spi = <64>;
        arm,msi-num-spis = <64>;
    };
};
```

### Reserved Memory (SWIOTLB)

```dts
reserved-memory {
    #address-cells = <2>;
    #size-cells = <2>;
    ranges;

    swiotlb: swiotlb@50000000 {
        compatible = "restricted-dma-pool";
        reg = <0x0 0x50000000 0x0 0x4000000>;  /* 64MB */
        reusable;
    };
};
```

## SWIOTLB Configuration

### Purpose

SWIOTLB (Software I/O Translation Lookaside Buffer) provides DMA bounce buffering for devices that cannot access all of guest physical memory.

### Why Needed

In the virtio-over-RPC architecture:
1. Device VM's QEMU accesses driver VM's memory via shared dataports
2. Only specific memory regions are shared
3. SWIOTLB ensures DMA targets accessible regions

### Boot Configuration

```
swiotlb=65536  # 256MB (65536 * 4KB pages)
```

### Device Tree Binding

PCI devices reference the SWIOTLB region:

```dts
pci@70000000 {
    /* ... */

    /* Reference restricted DMA pool for all PCI devices */
    memory-region = <&swiotlb>;
};
```

### Kernel Messages

Successful SWIOTLB initialization:
```
[    0.123456] software IO TLB: mapped [mem 0x50000000-0x53ffffff] (64MB)
```

## virtio Device Configuration

### Driver VM Setup

The driver VM sees virtio devices as PCI devices:

```bash
# List PCI devices
lspci -v

# Example output
00:01.0 Block storage controller: Red Hat, Inc. Virtio block device
00:02.0 Ethernet controller: Red Hat, Inc. Virtio network device
```

### virtio-blk

```bash
# Check block device
lsblk

# Mount filesystem
mount /dev/vda1 /mnt
```

### virtio-net

```bash
# Check network interface
ip link show

# Configure interface
ip addr add 192.168.1.10/24 dev eth0
ip link set eth0 up
```

### virtio-console

```bash
# Access console
screen /dev/hvc0
```

## Cross-VM Connector

### Device Detection

```bash
# Check for connector device
lspci | grep 1af4:a111

# Load module
modprobe connection

# Check UIO devices
ls /dev/uio*
```

### Userspace Access

```c
#include <fcntl.h>
#include <sys/mman.h>

int fd = open("/dev/uio0", O_RDWR);

// Map event register (BAR0)
void *event = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);

// Map dataport (BAR1)
void *data = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                  MAP_SHARED, fd, getpagesize());
```

## Boot Process

### Expected Boot Sequence

```
1. seL4 VMM creates VM
2. VMM generates device tree
3. VMM loads kernel and DTB into guest memory
4. VMM starts vCPU at kernel entry point
5. Kernel parses device tree
6. Kernel initializes SWIOTLB from reserved memory
7. PCI bus enumeration discovers virtio devices
8. virtio drivers probe and initialize devices
9. Init system starts
```

### Kernel Command Line

Typical command line for guest Linux:

```
console=ttyAMA0 earlycon=pl011,0x9000000 root=/dev/vda2 rw swiotlb=65536
```

| Parameter | Description |
|-----------|-------------|
| `console=ttyAMA0` | Serial console |
| `earlycon=pl011,0x9000000` | Early console for boot messages |
| `root=/dev/vda2` | Root filesystem on virtio disk |
| `swiotlb=65536` | SWIOTLB size |

## Device VM Specifics

### QEMU Installation

The device VM requires QEMU with seL4 accelerator:

```bash
# Verify QEMU installation
qemu-system-aarch64 --version

# Check for seL4 accelerator
qemu-system-aarch64 -accel help | grep sel4
```

### QEMU Guest Images

Device VM hosts images for nested VMs:

```
/var/lib/virt/images/
├── driver-vm.qcow2      # Driver VM disk image
└── user-vm.qcow2        # User VM disk image
```

### vhost Modules

```bash
# Load vhost modules
modprobe vhost
modprobe vhost_net
```

## Debugging

### Early Boot Issues

```bash
# Enable early console
earlycon=pl011,0x9000000

# Verbose kernel boot
loglevel=7 debug
```

### Device Tree Verification

```bash
# Dump device tree from running system
dtc -I fs -O dts /sys/firmware/devicetree/base

# Check specific nodes
cat /sys/firmware/devicetree/base/memory@40000000/reg | xxd
```

### SWIOTLB Issues

```bash
# Check SWIOTLB status
cat /sys/kernel/debug/swiotlb/io_tlb_nslabs
dmesg | grep -i swiotlb
```

### PCI Issues

```bash
# Verbose PCI enumeration
lspci -vvv

# Check PCI config space
hexdump /sys/bus/pci/devices/0000:00:01.0/config
```

### virtio Issues

```bash
# Check virtio device status
cat /sys/bus/virtio/devices/virtio0/status

# Verify driver binding
ls -la /sys/bus/virtio/devices/virtio0/driver
```

## Performance Tuning

### CPU Governor

```bash
# Set performance governor
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
```

### IRQ Affinity

```bash
# Set IRQ affinity for virtio device
echo 1 > /proc/irq/XX/smp_affinity
```

### Memory

```bash
# Disable transparent huge pages if causing issues
echo never > /sys/kernel/mm/transparent_hugepage/enabled
```

## Common Issues

| Issue | Cause | Solution |
|-------|-------|----------|
| No PCI devices | Missing device tree node | Check FDT generation |
| SWIOTLB errors | Region too small | Increase swiotlb size |
| virtio timeout | RPC communication failure | Check I/O proxy |
| UIO not working | Module not loaded | `modprobe connection` |

## Source Files

| File | Description |
|------|-------------|
| `projects/vm-linux/` | Guest Linux integration |
| `vm-images/meta-sel4/` | Yocto recipes |
| `configs/*.defconfig` | Kernel configurations |

## Related Documentation

- [Device Tree](../components/device-tree.md)
- [Memory Model](../architecture/memory-model.md)
- [Guest-Side Components](guest-side-components.md)
- [QEMU Backend](qemu-backend.md)
