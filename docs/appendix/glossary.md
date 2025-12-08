# Glossary

This glossary defines terms used throughout the TII seL4 virtio documentation.

## seL4 Terms

| Term | Definition |
|------|------------|
| **seL4** | Formally verified microkernel developed by CSIRO's Data61 |
| **Capability** | Token granting specific access rights to a kernel object |
| **CNode** | Capability Node - container for capabilities |
| **TCB** | Thread Control Block - kernel object representing a thread |
| **Endpoint** | IPC object for synchronous message passing |
| **Notification** | IPC object for asynchronous signaling |
| **Untyped** | Raw memory capability that can be retyped into other objects |
| **VSpace** | Virtual address space object |
| **ASID** | Address Space Identifier |

## CAmkES Terms

| Term | Definition |
|------|------------|
| **CAmkES** | Component Architecture for Microkernel-based Embedded Systems |
| **Component** | Self-contained unit with interfaces and implementation |
| **Connector** | Defines how components communicate |
| **Composition** | Instantiation of components and their connections |
| **Configuration** | Attribute values for component instances |
| **Dataport** | Shared memory region between components |
| **Event** | Asynchronous notification interface |
| **Procedure** | Synchronous RPC interface |

## Virtio Terms

| Term | Definition |
|------|------------|
| **virtio** | Standardized I/O virtualization framework |
| **virtqueue** | Circular buffer for device-driver communication |
| **vring** | Virtqueue ring buffer structure |
| **Driver** | Frontend that uses a virtio device (in guest VM) |
| **Device** | Backend that provides virtio functionality |
| **virtio-pci** | PCI transport for virtio devices |
| **virtio-blk** | Block device (disk) virtio type |
| **virtio-net** | Network device virtio type |

## Virtualization Terms

| Term | Definition |
|------|------------|
| **VMM** | Virtual Machine Monitor - manages guest VMs |
| **Hypervisor** | Software layer enabling virtualization |
| **Guest VM** | Virtual machine running on the hypervisor |
| **vCPU** | Virtual CPU assigned to a guest VM |
| **GPA** | Guest Physical Address |
| **HPA** | Host Physical Address |
| **MMIO** | Memory-Mapped I/O |
| **VM Exit** | Transition from guest to hypervisor |
| **VM Entry** | Transition from hypervisor to guest |

## ARM Terms

| Term | Definition |
|------|------------|
| **EL0** | Exception Level 0 - User mode |
| **EL1** | Exception Level 1 - OS kernel mode |
| **EL2** | Exception Level 2 - Hypervisor mode |
| **EL3** | Exception Level 3 - Secure monitor mode |
| **GIC** | Generic Interrupt Controller |
| **GICv2** | GIC version 2 |
| **GICv2m** | GICv2 with MSI support |
| **WFI** | Wait For Interrupt instruction |
| **WFE** | Wait For Event instruction |
| **HCR** | Hypervisor Configuration Register |
| **ESR** | Exception Syndrome Register |
| **VGIC** | Virtual GIC |

## PCI Terms

| Term | Definition |
|------|------------|
| **PCI** | Peripheral Component Interconnect |
| **PCIe** | PCI Express |
| **BAR** | Base Address Register |
| **ECAM** | Enhanced Configuration Access Mechanism |
| **CAM** | Configuration Access Mechanism |
| **MSI** | Message Signaled Interrupt |
| **MSI-X** | Extended MSI |
| **INTx** | Legacy PCI interrupts (INTA-INTD) |
| **Config Space** | PCI configuration registers |
| **BDF** | Bus/Device/Function address |

## Memory Terms

| Term | Definition |
|------|------------|
| **SWIOTLB** | Software I/O TLB - bounce buffer for DMA |
| **DMA** | Direct Memory Access |
| **Large Page** | 2MB memory page (vs 4KB normal page) |
| **TLB** | Translation Lookaside Buffer |
| **Page Table** | Data structure mapping virtual to physical addresses |
| **Frame** | Physical memory page |

## Project-Specific Terms

| Term | Definition |
|------|------------|
| **Device VM** | VM running QEMU, provides virtio backends |
| **Driver VM** | VM using virtio devices via drivers |
| **I/O Proxy** | VMM component handling I/O requests |
| **vso_rpc** | TII's RPC library for VM communication |
| **iobuf** | Shared buffer for RPC messages |
| **memdev** | Shared buffer for device data (SWIOTLB) |

## Build Terms

| Term | Definition |
|------|------------|
| **Yocto** | Embedded Linux build framework |
| **Bitbake** | Yocto's build tool |
| **Recipe** | Yocto build instructions for a package |
| **Layer** | Collection of Yocto recipes |
| **Poky** | Yocto reference distribution |
| **OpenEmbedded** | Build framework underlying Yocto |
| **CapDL** | Capability Distribution Language |

## Acronyms

| Acronym | Expansion |
|---------|-----------|
| TII | Technology Innovation Institute |
| SSRC | Secure Systems Research Center |
| HYPR | TII project code prefix |
| API | Application Programming Interface |
| RPC | Remote Procedure Call |
| IRQ | Interrupt Request |
| SPI | Shared Peripheral Interrupt |
| DTB | Device Tree Blob |
| FDT | Flattened Device Tree |
| DTS | Device Tree Source |
| SMP | Symmetric Multi-Processing |
| UART | Universal Asynchronous Receiver-Transmitter |

## Related Documentation

- [seL4 Documentation](https://docs.sel4.systems/)
- [CAmkES Documentation](https://docs.sel4.systems/projects/camkes/)
- [virtio Specification](https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html)
- [ARM Architecture Reference Manual](https://developer.arm.com/documentation/)
