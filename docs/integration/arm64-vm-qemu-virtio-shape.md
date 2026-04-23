# Arm64 `vm_qemu_virtio` Shape

This note records how the current Arm64 `vm_qemu_virtio` application is put
together, so the `qemu_x86_64` port can converge toward the same deployment
shape instead of inventing a parallel one.

## Scope

Authoritative source files for the current Arm64 shape:

- `apps/Arm/vm_qemu_virtio/CMakeLists.txt`
- `apps/Arm/vm_qemu_virtio/settings.cmake`
- `apps/Arm/vm_qemu_virtio/vm_qemu_virtio.camkes`
- platform device wiring under:
  - `apps/Arm/vm_qemu_virtio/qemu-arm-virt/devices.camkes`
  - `apps/Arm/vm_qemu_virtio/orinagx/devices.camkes`
  - `apps/Arm/vm_qemu_virtio/rpi4/devices.camkes`

This document is descriptive. Command authority remains in the runbooks.

## High-Level Shape

The Arm64 app is a two-VM Virtioso layout:

- `VM0` is the device-side VM
- `VM1` is the virtio-driver-side VM
- virtio transport between them is defined with `VIRTIO_COMPOSITION_DEF(0, 1)`

At the top level, the app declares:

- `DeclareVirtiosoCAmkESVM(VM0)`
- `DeclareVirtiosoCAmkESVM(VM1)`
- a single CAmkES rootserver from `vm_qemu_virtio.camkes`

The app-level CAmkES file stays small. Most topology-specific behavior lives in
the platform `devices.camkes` include.

## Build Inputs

The Arm64 app adds these guest boot artifacts to the file server:

- `linux`
- `linux-initrd`

Those resolve through the shared Virtioso helper variables:

- `VM_IMAGE_LINUX`
- `VM_IMAGE_INITRD`

For `qemu-arm-virt`, `settings.cmake` also defines simulation-time extra QEMU
arguments that attach the `vm-image-driver` disk image to the outer QEMU
machine:

- user-mode networking for VM0
- one virtio-net device
- one virtio-blk device
- driver disk path under `vm-images/build/tmp/deploy/images/<machine>/`

That disk is the host-side root filesystem for the driver/device-side guest
workflow.

## VM Boot Model

On Arm64 `qemu-arm-virt`:

- `VM0` boots with an initrd and a kernel command line rooted on `/dev/vda`
- `VM1` boots without an initrd and also expects a virtio block root

The per-platform `devices.camkes` file defines:

- RAM placement
- DTB/initrd/kernel load addresses
- virtio shared-memory regions
- guest-visible MMIO and IRQ layout
- DTB passthrough or DTB generation details

On physical platforms such as Orin AGX, the same high-level two-VM pattern
remains, but `devices.camkes` additionally carries the board-specific device
passthrough, IRQ routing, and console choices.

## Important Architectural Traits

The Arm64 app shape is notable for what it does **not** do in the app root:

- it does not embed Isengard-specific service logic
- it does not hardcode app-local business/runtime behavior
- it keeps platform device topology in platform-specific `devices.camkes`
- it keeps common app wiring in the top-level CMake/CAmkES pair

That is the shape the x86 `apps/x86/vm_qemu_virtio` target is supposed to
approach:

- two VMs
- one side booting the driver image
- the other side booting the user-facing image path
- no leftover early-Isengard service assumptions in the app itself

## Practical Mapping For x86_64

For `qemu_x86_64`, the closest Arm64 concepts are:

- Arm64 simulation extra args:
  become generated `images/qemu-extra-args` consumed by `tools/qemu_runner.py`
- Arm64 attached driver disk:
  becomes the packaged `vm-image-driver-qemux86-64.rootfs.*` image copied into
  the build `images/` directory
- Arm64 platform `devices.camkes`:
  becomes x86 machine-model and guest-boot wiring in the x86 app and shared
  x86 VMM support

The x86 port should therefore aim to mirror the Arm64 deployment contract,
while using x86-specific machine plumbing such as `q35`, ACPI, PCI config
space, and the remote-QEMU runtime artifact.
