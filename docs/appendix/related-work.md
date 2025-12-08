# Related Work and Background

This document provides background information and links to related projects and specifications.

## seL4 Microkernel

### Overview

seL4 is a formally verified microkernel with the following properties:

- **Functional correctness**: The C implementation matches its abstract specification
- **Integrity enforcement**: Processes cannot modify other processes' memory
- **Confidentiality**: Information cannot leak between isolated partitions
- **Capability-based access control**: All access mediated by unforgeable tokens

### Documentation

| Resource | URL |
|----------|-----|
| seL4 Website | https://sel4.systems/ |
| seL4 Manual | https://sel4.systems/Info/Docs/seL4-manual-latest.pdf |
| seL4 Tutorials | https://docs.sel4.systems/Tutorials/ |
| seL4 GitHub | https://github.com/seL4 |

### Key Concepts

**Capabilities:**
- Unforgeable tokens granting specific rights
- Passed by reference between components
- Enable fine-grained access control

**IPC (Inter-Process Communication):**
- Synchronous message passing
- Notification objects for async events
- Shared memory via frames

**Virtual Machines:**
- ARM hypervisor support (VHE and classic)
- vCPU abstraction
- Virtual interrupt controller

## CAmkES Framework

### Overview

CAmkES (Component Architecture for microkernel-based Embedded Systems) is a component framework for seL4:

- Declarative component specification
- Automatic code generation
- Predefined connector types
- Integration with seL4 capabilities

### Documentation

| Resource | URL |
|----------|-----|
| CAmkES Manual | https://docs.sel4.systems/CAmkES/ |
| CAmkES Tutorials | https://docs.sel4.systems/Tutorials/camkes-vm-linux.html |
| CAmkES GitHub | https://github.com/seL4/camkes-tool |

### Key Concepts

**Components:**
```camkes
component MyComponent {
    control;
    provides MyInterface api;
    dataport Buf shared_mem;
    emits Signal notify;
}
```

**Connectors:**
- `seL4RPCCall`: Synchronous RPC
- `seL4SharedData`: Shared memory
- `seL4Notification`: Async notifications
- `seL4GlobalAsynch`: Global async events

**Configurations:**
```camkes
configuration {
    mycomp.attribute = value;
}
```

## virtio Specification

### Overview

virtio is a standardized interface for virtual devices:

- Efficient I/O for virtual machines
- Minimal hypervisor-specific code
- Widely supported by guest operating systems

### Documentation

| Resource | URL |
|----------|-----|
| OASIS virtio Spec | https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html |
| virtio-blk | https://docs.oasis-open.org/virtio/virtio/v1.1/cs01/virtio-v1.1-cs01.html#x1-2390002 |
| virtio-net | https://docs.oasis-open.org/virtio/virtio/v1.1/cs01/virtio-v1.1-cs01.html#x1-1940001 |

### Key Concepts

**Virtqueues:**
- Ring buffers for data transfer
- Descriptor table + available ring + used ring
- Split and packed queue formats

**Device Types:**
| ID | Device |
|----|--------|
| 0 | Reserved |
| 1 | Network |
| 2 | Block |
| 3 | Console |
| 4 | Entropy (RNG) |
| 9 | 9P filesystem |

**Transport:**
- PCI (virtio-pci)
- MMIO (virtio-mmio)
- Channel I/O (virtio-ccw)

## ARM Virtualization

### Overview

ARMv8-A provides hardware virtualization support:

- EL2 (hypervisor) exception level
- Stage 2 address translation
- Virtual interrupts
- Timer virtualization

### Documentation

| Resource | URL |
|----------|-----|
| ARM Architecture Reference | https://developer.arm.com/documentation/ddi0487/latest |
| ARM Virtualization | https://developer.arm.com/documentation/102142/latest |

### Key Concepts

**Exception Levels:**
- EL0: User applications
- EL1: Guest OS kernel
- EL2: Hypervisor (seL4)
- EL3: Secure monitor

**Hypervisor Configuration Register (HCR_EL2):**
- Controls trapping behavior
- Virtual interrupt routing
- Memory virtualization

**Generic Interrupt Controller (GIC):**
- GICv2 with virtual CPU interface
- GICv3 with hardware virtualization list registers
- GICv2m for MSI support

## QEMU

### Overview

QEMU is an open-source machine emulator and virtualizer:

- Full system emulation
- Hardware virtualization (KVM)
- virtio device models

### Documentation

| Resource | URL |
|----------|-----|
| QEMU Documentation | https://www.qemu.org/docs/master/ |
| QEMU GitHub | https://github.com/qemu/qemu |
| QEMU Internals | https://qemu.readthedocs.io/en/latest/devel/index.html |

### Key Concepts

**Accelerators:**
- TCG: Software emulation
- KVM: Hardware virtualization
- seL4: TII custom accelerator

**Device Models:**
```bash
-device virtio-blk-pci,drive=hd0
-device virtio-net-pci,netdev=net0
```

**Memory Backends:**
```bash
-object memory-backend-file,id=mem,size=512M,mem-path=/dev/shm/vm
```

## Related Projects

### seL4 CAmkES VM

The base VM framework used by TII:

| Resource | URL |
|----------|-----|
| Repository | https://github.com/seL4/camkes-vm |
| Examples | https://github.com/seL4/camkes-vm-examples |
| Linux Integration | https://github.com/seL4/camkes-vm-linux |

### seL4 Core Platform

Alternative seL4 application framework:

| Resource | URL |
|----------|-----|
| Repository | https://github.com/seL4/sel4cp |
| Documentation | https://github.com/seL4/sel4cp/blob/main/docs/manual.md |

### ACRN Hypervisor

Similar architecture for embedded virtualization:

| Resource | URL |
|----------|-----|
| Website | https://projectacrn.org/ |
| GitHub | https://github.com/projectacrn/acrn-hypervisor |

## Academic References

### seL4 Verification

```
Klein, G., et al. "seL4: Formal verification of an OS kernel."
SOSP '09: Proceedings of the 22nd ACM Symposium on Operating Systems Principles.
```

### Capability Systems

```
Dennis, J. B., and Van Horn, E. C. "Programming semantics for multiprogrammed computations."
Communications of the ACM 9.3 (1966): 143-155.
```

### Virtualization

```
Popek, G. J., and Goldberg, R. P. "Formal requirements for virtualizable third generation architectures."
Communications of the ACM 17.7 (1974): 412-421.
```

### virtio

```
Russell, R. "virtio: towards a de-facto standard for virtual I/O devices."
ACM SIGOPS Operating Systems Review 42.5 (2008): 95-103.
```

## Standards and Specifications

| Standard | Description |
|----------|-------------|
| OASIS virtio | Virtual I/O device specification |
| PCI Express | PCIe base specification |
| ARM AMBA | ARM bus architecture |
| Device Tree | Flat device tree specification |
| ACPI | Advanced Configuration and Power Interface |

## Community Resources

### Mailing Lists

| List | URL |
|------|-----|
| seL4 Devel | https://lists.sel4.systems/hyperkitty/list/devel@sel4.systems/ |
| seL4 Announce | https://lists.sel4.systems/hyperkitty/list/announce@sel4.systems/ |

### Chat/Forums

| Platform | URL |
|----------|-----|
| seL4 Discourse | https://sel4.discourse.group/ |
| seL4 Mattermost | https://mattermost.trustworthy.systems/ |

### Conferences

| Conference | Focus |
|------------|-------|
| seL4 Summit | Annual seL4 community conference |
| USENIX Security | Security research |
| ACM CCS | Computer and communications security |

## Tools and Utilities

### Development Tools

| Tool | Purpose |
|------|---------|
| repo | Google's repository management |
| CMake | Build system |
| Ninja | Build executor |
| GDB | Debugging |
| QEMU | Emulation and testing |

### Analysis Tools

| Tool | Purpose |
|------|---------|
| Isabelle/HOL | Theorem prover (seL4 proofs) |
| CBMC | Bounded model checking |
| Frama-C | Static analysis |

## Source Files

This document references:
- seL4 kernel documentation
- CAmkES framework documentation
- virtio specification
- ARM architecture documentation
- QEMU documentation

## Related Documentation

- [Glossary](glossary.md)
- [Architecture Overview](../architecture/overview.md)
- [Virtio Architecture](../architecture/virtio-architecture.md)
