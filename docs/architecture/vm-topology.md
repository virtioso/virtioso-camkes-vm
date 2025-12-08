# VM Topology Architecture

This document provides a comprehensive guide to the flexible VM topology capabilities of the TII seL4 virtualization platform.

## Overview

Unlike traditional hypervisors (KVM, Xen, Hyper-V) that impose a fixed host-guest hierarchy, the seL4-based architecture supports **arbitrary VM relationship graphs**. This enables sophisticated compartmentalization strategies impossible with conventional virtualization.

## Fundamental Concepts

### VMs as Peers, Not Subjects

In traditional virtualization:
```
     Host (privileged)
         │
    ┌────┼────┐
    │    │    │
    ▼    ▼    ▼
  Guest Guest Guest  (all subordinate)
```

In seL4-based virtualization:
```
         seL4 (minimal TCB, policy-neutral)
                    │
    ┌───────────────┼───────────────┐
    │               │               │
    ▼               ▼               ▼
   VM1 ◄──────────► VM2 ◄─────────► VM3
        (peer relationships defined by CAmkES)
```

**Key insight**: seL4 doesn't designate any VM as "host". All VMs are peers with capabilities granted by CAmkES configuration.

### Device vs Driver Roles

A VM's role is determined by its CAmkES connections:

| Role | Responsibility | Hardware Access | Example |
|------|----------------|-----------------|---------|
| **Device VM** | Provides virtio backend | Physical peripherals | QEMU + vhost |
| **Driver VM** | Consumes virtio frontend | None (virtualized only) | Application workload |
| **Hybrid VM** | Both device AND driver | Selective | Chain intermediary |

## Topology Patterns

### Pattern 1: Simple Pair

The basic two-VM configuration:

```
┌─────────────────────────────────────────────────────────┐
│                         seL4                             │
│                                                          │
│    ┌─────────────────┐        ┌─────────────────┐       │
│    │    Device VM    │        │    Driver VM    │       │
│    │                 │        │                 │       │
│    │  Linux + QEMU   │        │  Linux + Apps   │       │
│    │  virtio-net ────┼────────┼─► eth0          │       │
│    │  virtio-blk ────┼────────┼─► /dev/vda      │       │
│    │                 │        │                 │       │
│    │  [NIC + eMMC]   │        │  [no hardware]  │       │
│    └─────────────────┘        └─────────────────┘       │
│                                                          │
│    1 device VM : 1 driver VM                             │
└─────────────────────────────────────────────────────────┘
```

**CAmkES:**
```camkes
VIRTIO_COMPOSITION_DEF(device, driver)
```

**Security**: Device VM compromise exposes driver VM's DMA buffers only.

### Pattern 2: Multiple Backends (N:1)

Multiple specialized device VMs serve one driver VM:

```
┌─────────────────────────────────────────────────────────────────────┐
│                              seL4                                    │
│                                                                      │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐                  │
│  │ Network VM  │  │ Storage VM  │  │  GPU VM     │                  │
│  │             │  │             │  │             │                  │
│  │ virtio-net ─┼──┼─────────────┼──┼─────────────┼───┐              │
│  │  [NIC]      │  │ virtio-blk ─┼──┼─────────────┼───┼───┐          │
│  └─────────────┘  │  [eMMC]     │  │ virtio-gpu ─┼───┼───┼───┐      │
│                   └─────────────┘  │  [GPU]      │   │   │   │      │
│                                    └─────────────┘   │   │   │      │
│                                                      │   │   │      │
│                                    ┌─────────────────▼───▼───▼──┐   │
│                                    │        Workload VM         │   │
│                                    │                            │   │
│                                    │  eth0    /dev/vda   GPU    │   │
│                                    │  [no hardware access]      │   │
│                                    └────────────────────────────┘   │
│                                                                      │
│  3 device VMs : 1 driver VM                                          │
└─────────────────────────────────────────────────────────────────────┘
```

**CAmkES:**
```camkes
VIRTIO_COMPOSITION_DEF(network, workload)
VIRTIO_COMPOSITION_DEF(storage, workload)
VIRTIO_COMPOSITION_DEF(gpu, workload)
```

**Security Benefits:**
- Network VM compromise: no storage/GPU access
- Storage VM compromise: no network/GPU access
- GPU VM compromise: no network/storage access
- All three must be compromised for full access

### Pattern 3: Shared Backend (1:N)

One device VM serves multiple isolated driver VMs:

```
┌─────────────────────────────────────────────────────────────────────┐
│                              seL4                                    │
│                                                                      │
│                   ┌─────────────────────────┐                        │
│                   │       Backend VM        │                        │
│                   │                         │                        │
│                   │  virtio-net (tenant-a) ─┼──────┐                 │
│                   │  virtio-net (tenant-b) ─┼──────┼──────┐          │
│                   │  virtio-blk (tenant-a) ─┼──────┼──────┼───┐      │
│                   │  virtio-blk (tenant-b) ─┼──────┼──────┼───┼───┐  │
│                   │                         │      │      │   │   │  │
│                   │  [NIC + eMMC]           │      │      │   │   │  │
│                   └─────────────────────────┘      │      │   │   │  │
│                                                    │      │   │   │  │
│        ┌───────────────────────────────────────────┘      │   │   │  │
│        │              ┌───────────────────────────────────┘   │   │  │
│        │              │              ┌────────────────────────┘   │  │
│        │              │              │              ┌─────────────┘  │
│        ▼              ▼              ▼              ▼                │
│  ┌──────────────────────────┐  ┌──────────────────────────┐         │
│  │       Tenant A VM        │  │       Tenant B VM        │         │
│  │                          │  │                          │         │
│  │   eth0       /dev/vda    │  │   eth0       /dev/vda    │         │
│  │   (isolated from B)      │  │   (isolated from A)      │         │
│  └──────────────────────────┘  └──────────────────────────┘         │
│                                                                      │
│  1 device VM : 2 driver VMs (isolated tenants)                       │
└─────────────────────────────────────────────────────────────────────┘
```

**Key Property**: Tenant A and Tenant B have completely separate dataports. They cannot see each other's traffic or storage.

### Pattern 4: Chain Topology

VMs form a pipeline where each is device for the next:

```
┌─────────────────────────────────────────────────────────────────────┐
│                              seL4                                    │
│                                                                      │
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────┐            │
│  │  Gateway VM │     │   App VM    │     │    DB VM    │            │
│  │  (Tier 0)   │     │  (Tier 1)   │     │  (Tier 2)   │            │
│  │             │     │             │     │             │            │
│  │ [Phys NIC] ─┼─────► virtio-net ─┼─────► virtio-net │            │
│  │             │     │             │     │             │            │
│  │  Internet   │     │ virtio-blk ─┼─────► virtio-blk │            │
│  │  facing     │     │ (provides)  │     │ (consumes) │            │
│  │             │     │             │     │             │            │
│  │             │     │  HYBRID:    │     │  [no HW]   │            │
│  │             │     │  driver AND │     │  Air-gapped│            │
│  │             │     │  device     │     │  from net  │            │
│  └─────────────┘     └─────────────┘     └─────────────┘            │
│                                                                      │
│  Gateway → App → DB  (each tier more isolated)                       │
└─────────────────────────────────────────────────────────────────────┘
```

**Security Tiers:**
1. **Gateway (Tier 0)**: Internet-facing, highest exposure, has NIC
2. **App (Tier 1)**: Only reachable via Gateway's virtio-net, provides storage to DB
3. **DB (Tier 2)**: Only reachable via App's virtio-blk, completely air-gapped from network

**Attack Progression Required:**
```
Internet → Exploit Gateway → Escape to App → Escape to DB
           (3 separate exploits needed)
```

### Pattern 5: Mesh Topology

Arbitrary peer-to-peer connections:

```
┌─────────────────────────────────────────────────────────────────────┐
│                              seL4                                    │
│                                                                      │
│        ┌─────────────┐                    ┌─────────────┐           │
│        │   Auth VM   │◄──── virtio-vsock ─┤   App VM    │           │
│        │             │                    │             │           │
│        │  Provides:  │                    │  Provides:  │           │
│        │  - OAuth    │                    │  - REST API │           │
│        │  - JWT      │                    │  - WebSocket│           │
│        └──────┬──────┘                    └──────┬──────┘           │
│               │                                  │                   │
│               │ virtio-vsock                     │ virtio-vsock      │
│               │                                  │                   │
│               ▼                                  ▼                   │
│        ┌─────────────┐                    ┌─────────────┐           │
│        │  Audit VM   │◄──── virtio-vsock ─┤  Cache VM   │           │
│        │             │                    │             │           │
│        │  Receives:  │                    │  Provides:  │           │
│        │  - All logs │                    │  - Redis    │           │
│        │  - Events   │                    │  - Memcached│           │
│        └─────────────┘                    └─────────────┘           │
│                                                                      │
│  Full mesh: any VM can connect to any other (as configured)          │
└─────────────────────────────────────────────────────────────────────┘
```

## CAmkES Connection Mechanics

### What a Connection Creates

Each `VIRTIO_COMPOSITION_DEF(device, driver)` creates:

```
Device VM                                      Driver VM
    │                                              │
    │  ┌──────────────────────────────────────┐    │
    ├──┤ iobuf dataport (4KB × 2)             ├────┤  RPC messages
    │  └──────────────────────────────────────┘    │
    │                                              │
    │  ┌──────────────────────────────────────┐    │
    ├──┤ memdev dataport (DMA pool / RAM)     ├────┤  Zero-copy data
    │  └──────────────────────────────────────┘    │
    │                                              │
    │  ┌──────────────────────────────────────┐    │
    ├──┤ notification (doorbell)              ├────┤  Async signals
    │  └──────────────────────────────────────┘    │
    │                                              │
    └──────────────────────────────────────────────┘
```

### Multiple Connections

A VM can have multiple connections:

```camkes
assembly {
    composition {
        component VM vm_hub;        // Central hub
        component VM vm_spoke_a;
        component VM vm_spoke_b;
        component VM vm_spoke_c;

        // Hub provides backends to all spokes
        VIRTIO_COMPOSITION_DEF(hub, spoke_a)
        VIRTIO_COMPOSITION_DEF(hub, spoke_b)
        VIRTIO_COMPOSITION_DEF(hub, spoke_c)
    }
}
```

Each connection has **independent** dataports - spoke_a cannot see spoke_b's data.

### Bidirectional Hybrid

A VM can be both device and driver:

```camkes
assembly {
    composition {
        component VM vm_upstream;
        component VM vm_middle;      // Hybrid
        component VM vm_downstream;

        // middle is DRIVER for upstream
        VIRTIO_COMPOSITION_DEF(upstream, middle)

        // middle is DEVICE for downstream
        VIRTIO_COMPOSITION_DEF(middle, downstream)
    }
}
```

## Peripheral Assignment

### Fine-Grained Hardware Access

Each VM receives only the hardware it needs:

```camkes
configuration {
    // Network backend: NIC only
    vm_network.pci_devices = [
        { name: "genet", bus: 1, dev: 0, fun: 0, irq: 189 }
    ];
    vm_network.untyped_mmios = [
        "0xfd580000:0x10000"   // GENET registers
    ];

    // Storage backend: eMMC only
    vm_storage.pci_devices = [
        { name: "emmc", bus: 2, dev: 0, fun: 0, irq: 158 }
    ];
    vm_storage.untyped_mmios = [
        "0xfe340000:0x100"    // eMMC registers
    ];

    // Workload: NO hardware
    vm_workload.pci_devices = [];
    vm_workload.untyped_mmios = [];
}
```

### Hardware Isolation Matrix

| VM | NIC | eMMC | GPU | USB | Consequence |
|----|-----|------|-----|-----|-------------|
| Network Backend | ✓ | ✗ | ✗ | ✗ | Can only affect network |
| Storage Backend | ✗ | ✓ | ✗ | ✗ | Can only affect storage |
| GPU Backend | ✗ | ✗ | ✓ | ✗ | Can only affect display |
| Workload VM | ✗ | ✗ | ✗ | ✗ | No direct HW access |

## Security Analysis

### Attack Surface per Topology

| Topology | Compromises Needed | Attack Surface |
|----------|-------------------|----------------|
| Single host (KVM) | 1 | Entire system |
| Simple pair | 1 | Device VM's hardware |
| N:1 backends | N | Each backend's hardware |
| 1:N shared | 1 | Backend's hardware (isolated tenants) |
| Chain | N | Must traverse each tier |
| Mesh | Varies | Only connected peers |

### VMSWIOTLB Impact on Topology

With `VMSWIOTLB=1`:
- Each connection exposes only 8MB DMA pool
- Driver VM private memory is never accessible
- Even with N connections, exposure is N × 8MB (not N × full RAM)

```
Multi-backend with VMSWIOTLB=1:
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│  Net Backend │     │ Blk Backend  │     │  GPU Backend │
│              │     │              │     │              │
│  Can see:    │     │  Can see:    │     │  Can see:    │
│  - NIC       │     │  - eMMC      │     │  - GPU       │
│  - 8MB DMA   │     │  - 8MB DMA   │     │  - 8MB DMA   │
│              │     │  (different  │     │  (different  │
│              │     │   pool!)     │     │   pool!)     │
└──────────────┘     └──────────────┘     └──────────────┘
        │                   │                   │
        └───────────────────┼───────────────────┘
                            │
                   ┌────────▼────────┐
                   │   Workload VM   │
                   │                 │
                   │  Private RAM:   │
                   │  NOT accessible │
                   │  by ANY backend │
                   └─────────────────┘
```

## Design Recommendations

### When to Use Each Pattern

| Pattern | Use When |
|---------|----------|
| **Simple pair** | Development, testing, simple deployments |
| **Multi-backend** | Security-critical with compartmentalized I/O |
| **Shared backend** | Multi-tenant, resource efficiency |
| **Chain** | DMZ, tiered security, compliance |
| **Mesh** | Microservices, distributed systems |

### Best Practices

1. **Minimize hardware per VM**: Each VM should have only the peripherals it absolutely needs
2. **Use VMSWIOTLB=1**: Always enable for security-sensitive deployments
3. **Air-gap sensitive data**: Place databases at the end of chains, not directly network-accessible
4. **Separate by trust level**: High-trust and low-trust workloads should not share backends
5. **Audit CAmkES connections**: Each connection is a potential attack path

## Source Files

| File | Description |
|------|-------------|
| `configurations/tii/vm.h` | VM composition macros |
| `apps/Arm/vm_qemu_virtio/` | Two-VM example |
| `apps/Arm/vm_virtio_multi_user/` | Multi-VM example |

## Related Documentation

- [System Overview](overview.md) - High-level architecture
- [Deployment Scenarios](../deployment/deployment-scenarios.md) - Concrete configurations
- [Memory Model](memory-model.md) - VMSWIOTLB and isolation
- [KVM/pKVM/seL4 Comparison](../appendix/kvm-pkvm-sel4-comparison.md) - Topology flexibility comparison
