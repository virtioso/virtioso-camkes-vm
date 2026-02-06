# Virtioso seL4 Virtio Virtualization Platform

This project extends the seL4 microkernel and CAmkES component framework to enable **virtio device virtualization** across multiple guest VMs. Virtio backends run inside guest Linux VMs using QEMU, providing a flexible and secure architecture for device sharing.

## What We Built

Technology Innovation Institute (Virtioso) developed a virtualization platform that:

- Runs **multiple isolated guest VMs** on the seL4 microkernel (ARM architecture)
- Enables **virtio devices** in driver VMs with backends served by device VMs
- Uses **QEMU** running inside a "device VM" to provide virtio device emulation
- Implements a custom **RPC protocol** for efficient VM-to-VM communication
- Supports **Raspberry Pi 4** and **QEMU ARM Virt** platforms

```mermaid
graph TB
    subgraph "seL4 Microkernel"
        subgraph "Device VM (Linux + QEMU)"
            QEMU[QEMU with virtio backends]
            VSO[vso_rpc library]
        end
        subgraph "Driver VM (Linux)"
            VIRTIO[virtio drivers]
            APP[Applications]
        end
        VMM[CAmkES VMM Components]
    end

    APP --> VIRTIO
    VIRTIO --> VMM
    VMM --> VSO
    VSO --> QEMU
```

## Key Features

| Feature | Description |
|---------|-------------|
| **VM Isolation** | seL4's capability-based security ensures strong isolation between VMs |
| **Flexible Backends** | Virtio backends run in userspace (QEMU), easy to configure and extend |
| **Standard Interfaces** | Uses standard Linux virtio drivers in guest VMs |
| **Large Page Support** | 2MB page mappings for improved performance |
| **PCIe/ECAM Support** | Full PCIe extended configuration space support |

## Documentation Sections

### Start Here
- [Prerequisites](start-here/prerequisites.md) - Development environment setup
- [Building](start-here/building.md) - Build instructions
- [Running on QEMU](start-here/running-qemu.md) - QEMU ARM Virt instructions
- [Running on RPi4](start-here/running-rpi4.md) - Raspberry Pi 4 deployment
- [Kernel Development Workflow](start-here/kernel-development-workflow.md) - Yocto devtool workflow

### Architecture
- [System Overview](architecture/overview.md) - High-level architecture and component relationships
- [VM Topology](architecture/vm-topology.md) - Flexible N:M topology and compartmentalization
- [Real-Time Properties](architecture/real-time-properties.md) - WCET guarantees and safety certification
- [Virtio Architecture](architecture/virtio-architecture.md) - How virtio devices span VMs
- [RPC Protocol](architecture/rpc-protocol.md) - VM-to-VM communication protocol
- [Memory Model](architecture/memory-model.md) - Shared memory and SWIOTLB
- [vhost Acceleration](architecture/vhost-acceleration.md) - Why MSI is essential for KVM-like performance

### Components
- [I/O Proxy](components/io-proxy.md) - Core I/O request handling infrastructure
- [PCI Passthrough](components/pci-passthrough.md) - PCI device virtualization
- [Interrupt Handling](components/interrupt-handling.md) - IRQ, MSI, and GICv2m emulation
- [Device Tree](components/device-tree.md) - FDT generation for guest VMs
- [CAmkES Templates](components/camkes-templates.md) - Component templates for virtio VMs
- [Hyp Ftrace Interface](components/hyp-ftrace-interface.md) - Hypervisor ftrace control interface
- [Device Tree Passthrough](components/passthru.md) - DTB passthrough analysis

### Integration
- [seL4 Kernel Modifications](integration/kernel-modifications.md) - Virtioso changes to seL4 kernel
- [QEMU Backend](integration/qemu-backend.md) - QEMU seL4 accelerator details
- [QEMU seL4 Accelerator](integration/qemu-sel4-accelerator.md) - QEMU accel/sel4 implementation details
- [kmod-sel4-virt](integration/kmod-sel4-virt.md) - Kernel module providing KVM-like API
- [Guest-Side Components](integration/guest-side-components.md) - Code running inside guest VMs
- [Guest Linux Configuration](integration/guest-linux.md) - Guest kernel and driver setup
- [Other Repository Changes](integration/other-repos.md) - Changes across Virtioso repositories

### Build System
- [Build Architecture](build-system/build-architecture.md) - Docker, Make, CMake, Yocto
- [Yocto Integration](build-system/yocto-integration.md) - Guest image generation
- [CI/CD](build-system/ci-cd.md) - GitHub Actions workflows

### Deployment
- [Deployment Scenarios](deployment/deployment-scenarios.md) - Multi-VM configurations
- [Production Guide](deployment/production-guide.md) - Performance and security

### Platforms
- [Orin AGX RAS Errors](platforms/orin-agx/ras-errors.md) - Canonical RAS problem statement and status
- [Orin AGX Debugging Guide](platforms/orin-agx/orin-agx-debugging-guide.md) - Start here for Orin AGX work
- [Orin AGX Complete Debugging Reference](platforms/orin-agx/orin-agx-complete-debugging-reference.md)
- [Orin AGX Cache Investigation](platforms/orin-agx/investigations/orin-cache-investigation.md)
- [Orin AGX Porting Guide](platforms/orin-agx/porting/orinagx-camkes-porting.md)
- [Orin AGX: vm_qemu_virtio Port](platforms/orin-agx/porting/vm-qemu-virtio-orinagx.md)
- [Orin AGX: vm-image-minimal Port](platforms/orin-agx/porting/vm-image-minimal-orinagx.md)

### Reference
- [API Reference](reference/api-reference.md) - Data structures and functions
- [RPC Opcodes](reference/rpc-opcodes.md) - Complete opcode reference
- [Configuration](reference/configuration.md) - CMake and CAmkES options
- [Kernel Notes](reference/kernel/upstream-addrFromKPPtr-bugs.md) - Kernel-specific notes
- [Release Instructions](reference/release/release_instructions.md)

### Plans
- [VM Image Boot Unification](plans/vm-image-boot-unification-plan.md)
- [CAmkES to Microkit Migration](plans/camkes-to-microkit-migration.md)
- [DTB Generation Enhancements](plans/dtb-generation-enhancements-plan.md)
- [Ftrace Upstream Integration Plan](plans/ftrace-upstream-integration-plan.md)
- [capdl Autopilot Extension](plans/capdl-autopilot-extension-plan.md)

### Appendix
- [KVM vs pKVM vs seL4](appendix/kvm-pkvm-sel4-comparison.md) - Virtualization approach comparison
- [Glossary](appendix/glossary.md) - Terms and definitions
- [Related Work](appendix/related-work.md) - Background reading

### Misc
- [Native GPU/Host1x Experiment](misc/experimental/native-gpu-host1x.md)
- [Logo Prompt](misc/logo-prompt.md)

## Quick Start

```bash
# Clone and sync repositories
repo init -u git@github.com:virtioso/virtioso-manifest.git -b virtioso/development
repo sync

# Build Docker container
make docker

# Configure for Raspberry Pi 4
make raspberrypi4-64_defconfig

# Build guest Linux images
make linux-image

# Build CAmkES VM application
make vm_qemu_virtio
```

See [Getting Started](start-here/prerequisites.md) for detailed instructions.

## Repository Structure

| Repository | Description |
|------------|-------------|
| `projects/virtioso-camkes-vm` | Main Virtioso project (this documentation) |
| `kernel/` | seL4 kernel with Virtioso modifications |
| `projects/sel4_projects_libs/` | VMM libraries with large page, PCIe, IRQ enhancements |
| `projects/vm-linux/` | Guest Linux integration |
| `sources/kmod-sel4-virt/` | Kernel module bridging QEMU and seL4 RPC |
| `sources/qemu-sel4-virtio/` | QEMU with seL4 accelerator for virtio backends |
| `virtioso-build/` | Build system and Docker |
| `vm-images/meta-sel4/` | Yocto layer for guest images |

## Supported Platforms

| Platform | Status | Notes |
|----------|--------|-------|
| QEMU ARM Virt | Supported | Development and testing |
| Raspberry Pi 4 | Supported | Production target |

## License

This project is part of the seL4 ecosystem. See individual repositories for license information.

## Contributing

Contributions are welcome. Please see the [build instructions](start-here/building.md) to set up your development environment.
