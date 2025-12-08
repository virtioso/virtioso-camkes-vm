# Running on QEMU

This document describes how to run the TII seL4 virtio platform on QEMU ARM Virt.

## Prerequisites

- Completed [build](building.md) for QEMU platform
- QEMU ARM installed (included in Docker container)

## Quick Start

### Using Make Target

```bash
cd $WORKSPACE

# Configure for QEMU
make qemuarm64_defconfig

# Build
make vm_qemu_virtio

# Run simulation
make simulate_vm_qemu_virtio
```

### Manual QEMU Invocation

```bash
cd $WORKSPACE/qemu_vm_qemu_virtio

qemu-system-aarch64 \
    -machine virt,virtualization=on,highmem=off,secure=off \
    -cpu cortex-a57 \
    -m 2048 \
    -nographic \
    -serial mon:stdio \
    -kernel images/capdl-loader-image-arm-qemu-arm-virt
```

## Boot Sequence

### Expected Output

```
ELF-loader started on CPU: ARM Ltd. Cortex-A57 r1p3
  paddr=[...]
Bringing up 1 other CPU(s)
Switching to a]hypervisor mode
Bootstrapping kernel
seL4 microkernel (version: ...)
...
[vm0] Linux version 5.x.x ...
[vm1] Linux version 5.x.x ...
```

### Boot Stages

1. **ELF Loader**: Loads seL4 kernel and root task
2. **seL4 Kernel**: Initializes microkernel
3. **CAmkES Root**: Starts VMM components
4. **Device VM (VM0)**: Boots Linux, starts QEMU
5. **Driver VM (VM1)**: Boots Linux with virtio devices

## Interacting with VMs

### Serial Console

The simulation multiplexes serial output from both VMs:

```
[vm0] device-vm login:
[vm1] driver-vm login:
```

### Switching Consoles

With `-serial mon:stdio`, use QEMU monitor:

```
# Switch to QEMU monitor
Ctrl-A C

# In monitor, switch serial
(qemu) info chardev
(qemu) chardev-send-break serial0
```

### Login Credentials

Default credentials for guest VMs:

| VM | Username | Password |
|----|----------|----------|
| Device VM | root | (none) |
| Driver VM | root | (none) |

## Networking

### User-Mode Networking

Add to QEMU command line:

```bash
-netdev user,id=net0,hostfwd=tcp::2222-:22 \
-device virtio-net-pci,netdev=net0
```

### SSH Access

```bash
ssh -p 2222 root@localhost
```

## Debugging

### GDB Server

Start QEMU with GDB:

```bash
qemu-system-aarch64 \
    ... \
    -s -S  # GDB on port 1234, wait for connection
```

Connect GDB:

```bash
aarch64-linux-gnu-gdb
(gdb) target remote :1234
(gdb) continue
```

### Kernel Symbols

```bash
(gdb) symbol-file kernel/build/kernel.elf
(gdb) break handleVCPUFault
```

### QEMU Tracing

Enable trace events:

```bash
qemu-system-aarch64 \
    ... \
    -d guest_errors,unimp \
    -D qemu.log
```

## Performance

### CPU Cores

Add more cores:

```bash
-smp 4
```

### Memory

Adjust memory:

```bash
-m 4096  # 4GB
```

### KVM Acceleration

On ARM64 hosts with KVM:

```bash
-enable-kvm \
-cpu host
```

## Common Issues

### "Could not load kernel"

Check image path:

```bash
ls -la images/capdl-loader-image-arm-qemu-arm-virt
```

### VM Doesn't Boot

Check for errors in output:

```
-d guest_errors -D errors.log
```

### Slow Performance

Enable KVM if available:

```bash
# Check KVM support
ls /dev/kvm

# Run with KVM
qemu-system-aarch64 -enable-kvm ...
```

## QEMU Options Reference

| Option | Description |
|--------|-------------|
| `-machine virt` | ARM Virt machine type |
| `-cpu cortex-a57` | CPU model |
| `-m 2048` | Memory in MB |
| `-nographic` | No graphical output |
| `-serial mon:stdio` | Serial to terminal |
| `-kernel` | Kernel/loader image |
| `-s` | GDB server on :1234 |
| `-S` | Wait for GDB connection |
| `-d` | Debug/trace options |
| `-D` | Debug output file |

## Next Steps

- [Running on RPi4](running-rpi4.md) for hardware deployment
- [Debugging](../reference/debugging.md) for troubleshooting
