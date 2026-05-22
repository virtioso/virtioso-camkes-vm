# Running on QEMU

This document describes the manual QEMU workflows for Virtioso seL4 builds.
The canonical runner is:

- tools/qemu_runner.py

It prefers the generated seL4 `simulate` script and overrides the QEMU binary
to use the Yocto-built custom QEMU.

For x86_64, the preferred runtime source is now the deployed relocatable
runtime artifact under:

- `vm-images/build/tmp/deploy/virtioso-qemu-runtime/`

Build it from the workspace root with:

- `make qemu-runtime-x86_64`

The runner can also consume:

- an explicitly unpacked runtime tree via `--runtime-dir`
- an explicit runtime tarball via `--runtime-tar`

The old direct `tmp/work/.../qemu-system-native` lookup remains only as a
compatibility fallback.

## Prerequisites

- Completed [build](building.md) for QEMU platform
- QEMU ARM installed (included in Docker container)

## Quick Start

### Build A QEMU ARM64 Target

```bash
cd $WORKSPACE

make qemu_arm64_defconfig
make vm_qemu_virtio
```

### Run QEMU ARM64 Locally

Use the repo-owned runner:

```bash
cd $WORKSPACE
python3 projects/virtioso-camkes-vm/tools/qemu_runner.py \
    run-local \
    --target qemu_arm64_defconfig \
    --binary "$WORKSPACE/qemuarm64_vm_qemu_virtio/images/capdl-loader-image-arm-qemu-arm-virt"
```

The runner will:

- locate the sibling generated `simulate` script
- locate the Yocto-built custom `qemu-system-aarch64`
- accept either the installed native sysroot layout or the Yocto recipe build tree
- set the required library path from the Yocto sysroot
- execute the generated `simulate` script with the custom QEMU

### Prepare And Run QEMU PC99 Remotely

Build the target first:

```bash
cd $WORKSPACE
make qemu_x86_64_defconfig
make sel4test
```

Print a config template and create `~/.virtioso-qemu-runners.json`:

```bash
python3 projects/virtioso-camkes-vm/tools/qemu_runner.py print-config-template
```

Run the remote workflow:

```bash
cd $WORKSPACE
python3 projects/virtioso-camkes-vm/tools/qemu_runner.py \
    run-remote \
    --target qemu_x86_64_defconfig \
    --binary "$WORKSPACE/qemu_x86_64_sel4test/images/sel4test-driver-image-x86_64-pc99"
```

The remote workflow will:

- package the generated `simulate` runtime tree
- prefer the deployed x86_64 runtime artifact from `tmp/deploy/virtioso-qemu-runtime`
- fall back to the Yocto `qemu-system-native` workdir only if no runtime artifact is available
- bundle the custom `qemu-system-x86_64` runtime plus only its required Yocto-side libraries and QEMU data files
- transfer the bundle to the configured Intel host over SSH
- execute the bundle remotely in the foreground
- terminate when the test or SSH session ends

## Generated `simulate` Script

The preferred manual path is the generated sibling `simulate` script in the
build directory. The runner uses the supported `-b` argument to override the
QEMU binary:

```bash
cd "$WORKSPACE/qemuarm64_vm_qemu_virtio"
./simulate -b \
  "$WORKSPACE/vm-images/build/tmp/work/x86_64-linux/qemu-system-native/9.2.0/recipe-sysroot-native/usr/bin/qemu-system-aarch64"
```

This keeps the invocation aligned with seL4’s generated launch script instead
of replacing it with a separate wrapper-specific QEMU command line.

When an explicit runtime artifact is provided or a deployed runtime artifact is
available under `tmp/deploy/virtioso-qemu-runtime`, the runner uses that
runtime instead of resolving QEMU from `tmp/work/...`.

If no runtime artifact is available and the native recipe did not install the
QEMU binary under `recipe-sysroot-native/usr/bin`, the runner falls back to the
sibling `build/qemu-system-*` binary and still reuses the matching
`recipe-sysroot-native/usr/lib*` runtime libraries.

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

## Interacting With VMs

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

### "simulate script not found"

The runner expects a generated sibling `simulate` script in the build
directory. Rebuild the target if it is missing.

### Custom QEMU not found

The runner resolves the custom QEMU from the Yocto work tree under:

```bash
vm-images/build/tmp/work/x86_64-linux/qemu-system-native/*/recipe-sysroot-native/usr/bin
```

If this tree is missing, rebuild the Yocto-side QEMU outputs first.

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
