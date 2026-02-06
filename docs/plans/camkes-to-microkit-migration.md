# CAmkES to Microkit Migration Investigation

**Status**: Investigation / Planning
**Context**: The seL4 community is transitioning from CAmkES to seL4 Microkit as the recommended application framework.

## Executive Summary

This document investigates the feasibility and approach for migrating the TII seL4 virtio virtualization platform from CAmkES to seL4 Microkit. The migration would align the project with the seL4 community's direction while potentially simplifying the build system and reducing toolchain complexity.

## Background

### What is seL4 Microkit?

seL4 Microkit (formerly "seL4 Core Platform") is a lightweight framework for building systems on seL4:

- **Simpler than CAmkES**: Fewer abstractions, more explicit control
- **XML-based system description**: Replaces CAmkES DSL (`.camkes` files)
- **No code generation for IPC stubs**: Manual but straightforward IPC
- **Focus on static systems**: Fixed configuration at build time
- **Official seL4 Foundation project**: Active development and support

```
CAmkES Stack:                    Microkit Stack:
┌──────────────────────┐         ┌──────────────────────┐
│    .camkes files     │         │   system.xml         │
│   (custom DSL)       │         │   (standard XML)     │
├──────────────────────┤         ├──────────────────────┤
│  CAmkES Python tools │         │  microkit tool       │
│  (code generation)   │         │  (minimal codegen)   │
├──────────────────────┤         ├──────────────────────┤
│  .template.c files   │         │  (not needed)        │
│  (Jinja2 templates)  │         │                      │
├──────────────────────┤         ├──────────────────────┤
│  Generated glue code │         │  libmicrokit.a       │
│  (per-component)     │         │  (standard library)  │
├──────────────────────┤         ├──────────────────────┤
│       seL4           │         │       seL4           │
└──────────────────────┘         └──────────────────────┘
```

### Why Consider Migration?

| Aspect | CAmkES | Microkit |
|--------|--------|----------|
| **Community Direction** | Legacy, maintenance mode | Actively developed |
| **Toolchain Complexity** | High (Python, Jinja2, custom parser) | Low (single tool) |
| **Build Time** | Slower (code generation) | Faster |
| **Learning Curve** | Steeper (custom DSL) | Gentler (familiar XML) |
| **Flexibility** | More abstractions | More explicit control |
| **VM Support** | Mature (libsel4vm) | Evolving (libvmm) |

## Current CAmkES Usage Analysis

### CAmkES Features Used in virtioso-camkes-vm

**1. Component Definitions**

```camkes
// Current: CAmkES component definition
component VM0 {
    VM_TII_INIT_DEF()
    VIRTIO_DRIVER_COMPONENT_DEF(1)
}

component VM1 {
    VM_TII_INIT_DEF()
    VIRTIO_DEVICE_COMPONENT_DEF(0)
}
```

**2. Connection Types**

| CAmkES Connection | Purpose | Microkit Equivalent |
|-------------------|---------|---------------------|
| `seL4SharedDataWithCaps` | Shared memory (iobuf, memdev) | Memory region + channel |
| `seL4GlobalAsynch` | Async notification (upcall) | Notification channel |
| `seL4Notification` | Notification (downcall) | Notification channel |
| `seL4VMDTBPassthrough` | DTB handling | Custom handling |

**3. Macro-Based Configuration**

```c
// configurations/virtioso/vm.h - Heavy macro usage
#define VIRTIO_COMPOSITION_DEF(_dev, _drv) \
    connection seL4SharedDataWithCaps vm##_dev##_vm##_drv##_iobuf(...); \
    connection seL4SharedDataWithCaps vm##_dev##_vm##_drv##_memdev(...); \
    connection seL4GlobalAsynch vm##_dev##_vm##_drv##_upcall(...); \
    connection seL4Notification vm##_dev##_vm##_drv##_downcall(...);
```

**4. Template Files (Code Generation)**

```c
// templates/seL4VirtIODeviceVM.template.c
/*- set vm_virtio_drivers = configuration[me.name].get('vm_virtio_drivers') -*/
/*- for drv in vm_virtio_drivers -*/
extern dataport_caps_handle_t vm/*? drv.id ?*/_iobuf_handle;
/*- endfor -*/
```

**5. Configuration Attributes**

```camkes
configuration {
    vm0.cnode_size_bits = 23;
    vm0.base_prio = 100;
    vm0._priority = 121;
    vm0.simple_untyped24_pool = 12;
    // ... many more attributes
}
```

### Dependencies on CAmkES Infrastructure

| Dependency | Usage | Migration Impact |
|------------|-------|------------------|
| `camkes.h` | Component runtime | Replace with libmicrokit |
| `dataport_caps_handle_t` | Shared memory handles | Microkit memory regions |
| `seL4_*_emit()` | Notification sending | `microkit_notify()` |
| `*_notification_badge()` | Badge retrieval | Channel ID |
| `register_async_event_handler()` | Event registration | `microkit_msginfo` loop |
| CAmkES build system | CMake integration | Microkit build |

## Microkit Concepts Mapping

### System Description

**CAmkES** (vm_qemu_virtio.camkes):
```camkes
assembly {
    composition {
        VM_COMPOSITION_DEF(0)
        VM_COMPOSITION_DEF(1)
        VIRTIO_COMPOSITION_DEF(0, 1)
        connection seL4VMDTBPassthrough vm0_dtb(...);
    }
    configuration {
        vm0.cnode_size_bits = 23;
        vm0.base_prio = 100;
    }
}
```

**Microkit** (equivalent system.xml):
```xml
<?xml version="1.0" encoding="UTF-8"?>
<system>
    <!-- VM0: Driver VM -->
    <protection_domain name="vm0" priority="121" budget="1000" period="10000">
        <program_image path="vm0.elf" />
        <map mr="vm0_ram" vaddr="0x40000000" perms="rw" cached="true" />
        <map mr="iobuf_0_1" vaddr="0x50000000" perms="rw" />
        <map mr="memdev_0_1" vaddr="0x51000000" perms="rw" />
    </protection_domain>

    <!-- VM1: Device VM -->
    <protection_domain name="vm1" priority="101" budget="1000" period="10000">
        <program_image path="vm1.elf" />
        <map mr="vm1_ram" vaddr="0x40000000" perms="rw" cached="true" />
        <map mr="iobuf_0_1" vaddr="0x50000000" perms="rw" />
        <map mr="memdev_0_1" vaddr="0x51000000" perms="rw" />
    </protection_domain>

    <!-- Shared Memory Regions -->
    <memory_region name="iobuf_0_1" size="0x2000" />
    <memory_region name="memdev_0_1" size="0x1000000" />

    <!-- Notification Channels -->
    <channel>
        <end pd="vm0" id="1" />  <!-- vm0 notifies vm1 -->
        <end pd="vm1" id="1" />
    </channel>
    <channel>
        <end pd="vm1" id="2" />  <!-- vm1 notifies vm0 -->
        <end pd="vm0" id="2" />
    </channel>
</system>
```

### IPC and Notifications

**CAmkES**:
```c
// Generated emit function
vm1_ntfn_send_emit();

// Event handler registration
register_async_event_handler(badge, callback, cookie);
```

**Microkit**:
```c
#include <microkit.h>

// Notification sending
microkit_notify(CHANNEL_VM1);

// Event handling (in required notified() function)
void notified(microkit_channel ch) {
    switch (ch) {
        case CHANNEL_FROM_VM1:
            handle_vm1_notification();
            break;
    }
}
```

### Shared Memory Access

**CAmkES**:
```c
// Template-generated handle
extern dataport_caps_handle_t vm1_iobuf_handle;

// Access via dataport
void *iobuf = (void *)vm1_iobuf;
```

**Microkit**:
```c
// Defined in system.xml, mapped to fixed vaddr
#define IOBUF_VADDR 0x50000000

void *iobuf = (void *)IOBUF_VADDR;
```

## Migration Challenges

### 1. VM Support Maturity

CAmkES has mature VM support via `libsel4vm`. Microkit's VM support (`libvmm`) is less mature:

| Feature | CAmkES/libsel4vm | Microkit/libvmm |
|---------|------------------|-----------------|
| ARM64 VMs | Mature | Developing |
| vCPU management | Complete | Basic |
| Memory management | Flexible | Simpler |
| Device passthrough | Supported | Limited |
| Interrupt injection | Full | Partial |

**Mitigation**: May need to port/adapt libsel4vm features to Microkit or contribute to libvmm.

### 2. Template Code Generation

CAmkES templates generate per-configuration code. Microkit doesn't have templates:

```c
// Current: Template generates connection array
/*- for drv in vm_virtio_drivers -*/
    { &vm/*? drv.id ?*/_iobuf_handle, ... },
/*- endfor -*/
```

**Microkit approach**: Use build-time code generation (Python/shell script) or compile-time macros.

### 3. Dynamic Configuration

CAmkES allows complex configuration expressions. Microkit is more static:

```camkes
// CAmkES: Complex expressions
vm0.simple_untyped24_pool = 2 + (VM0_RAM_SIZE >> 24);
```

**Microkit approach**: Compute values at build time, put in XML or header.

### 4. N:M VM Topology

The flexible VM topology (N device VMs : M driver VMs) maps well to Microkit:

```xml
<!-- 2 Device VMs serving 1 Driver VM -->
<channel>
    <end pd="device_vm_net" id="1" />
    <end pd="driver_vm" id="1" />
</channel>
<channel>
    <end pd="device_vm_blk" id="2" />
    <end pd="driver_vm" id="2" />
</channel>
```

This is actually **cleaner** in Microkit - explicit channel IDs make topology visible.

### 5. Build System Integration

CAmkES deeply integrates with CMake. Microkit has its own build:

```
Current:                          Microkit:
CMakeLists.txt                    Makefile (or CMake wrapper)
  └── CAmkES tools                  └── microkit tool
      └── Generate code                 └── Build system image
          └── Compile                       └── Compile PDs
```

## Migration Strategy

### Phase 1: Parallel Prototype (Low Risk)

Create a minimal Microkit port alongside CAmkES:

1. Simple 2-VM configuration (driver + device)
2. Shared memory for iobuf/memdev
3. Basic notification
4. No QEMU, just ping-pong test

**Goal**: Validate Microkit can express the core pattern.

### Phase 2: VM Library Evaluation

Evaluate libvmm vs porting libsel4vm:

1. What does libvmm provide?
2. What's missing for our use case?
3. Effort to port vs contribute upstream?

**Goal**: Decide on VM library strategy.

### Phase 3: Feature Parity Port

Port full functionality:

1. Multi-VM configurations
2. virtio backend operation
3. PCI passthrough
4. Interrupt handling

**Goal**: Full feature parity with CAmkES version.

### Phase 4: Build System Migration

Update build infrastructure:

1. Yocto integration for Microkit
2. Docker container updates
3. CI/CD pipeline changes

**Goal**: Production-ready build.

## Effort Estimation

| Task | Complexity | Effort | Notes |
|------|------------|--------|-------|
| Basic Microkit prototype | Low | 1-2 weeks | Ping-pong VMs, validate pattern |
| libvmm evaluation | Medium | 1 week | Already analyzed (see above) |
| Fault handling adapter | Low | 1 week | ~50 lines wrapper code |
| Cross-VM PCI (Option B) | Medium | 1 week | ~200-300 lines new code |
| Template elimination | Low | 1 week | Python generator or static |
| vpci/gicv2m port (optional) | Medium | 2-3 weeks | For full PCI passthrough |
| Build system changes | Medium | 1-2 weeks | Parallel Microkit build |
| RPC queue integration | Low | 1 week | Code unchanged, just glue |
| Testing & validation | Medium | 2-3 weeks | virtio-net, virtio-blk |
| **Total (minimal)** | | **8-10 weeks** | Cross-VM only |
| **Total (full PCI)** | | **12-15 weeks** | With vpci/gicv2m |

## Recommendation

### Short Term

- **Continue with CAmkES** for production
- **Create Microkit prototype** for evaluation
- **Monitor libvmm development**

### Medium Term

- **Contribute to libvmm** if gaps identified
- **Develop migration tooling** (CAmkES → Microkit)
- **Dual-build support** during transition

### Long Term

- **Complete migration** when Microkit VM support matures
- **Deprecate CAmkES version** after validation

## Benefits of Migration

1. **Simpler toolchain**: No Python/Jinja2 code generation
2. **Faster builds**: Less intermediate steps
3. **Community alignment**: Active development, better support
4. **Clearer system description**: XML is explicit, no macro magic
5. **Easier debugging**: Less generated code to trace through

## Risks of Migration

1. **VM support maturity**: libvmm less proven than libsel4vm
2. **Migration effort**: Significant engineering investment
3. **Dual maintenance**: Period of supporting both
4. **Feature gaps**: May discover CAmkES-specific features hard to replicate

## Architectural Insight: Inter-VM RPC, sDDF, and virtio

Before diving into challenges, it's important to understand the layer separation:

### The Three Control/Data Planes

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         GUEST LINUX (unmodified)                             │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│   VIRTIO CONTROL PLANE              │    VIRTIO DATA PLANE                  │
│   (virtio-pci / virtio-mmio)        │    (virtqueues / vring)               │
│   • Device discovery                │    • Shared memory rings              │
│   • Feature negotiation             │    • Descriptor/available/used        │
│   • Config space R/W                │    • Zero-copy I/O buffers            │
│                                     │                                        │
│   ──── DO NOT MODIFY ────────────────────── DO NOT MODIFY ────              │
│                                                                              │
└───────────────────────────────────────┬─────────────────────────────────────┘
                                        │
        MMIO faults (trapped by VMM)    │    Shared memory (memdev dataport)
                                        │
┌───────────────────────────────────────┼─────────────────────────────────────┐
│                              seL4 VMM │                                      │
│                                       ▼                                      │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │              INTER-VM RPC (TII's "VM Admin" Protocol)                │   │
│   │                                                                      │   │
│   │   This is a THIRD control plane - for VMM ↔ Backend communication   │   │
│   │                                                                      │   │
│   │   Protocol:           Transport (sDDF-like):                        │   │
│   │   • QEMU_OP_MMIO      • Shared memory queues (iobuf)                │   │
│   │   • QEMU_OP_SET_IRQ   • Async notifications (doorbells)             │   │
│   │   • QEMU_OP_REGISTER  • Lock-free ring buffers                      │   │
│   │                                                                      │   │
│   │   ──── THIS IS OURS - can be adapted ────                           │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                       │                                      │
└───────────────────────────────────────┼──────────────────────────────────────┘
                                        │
                                        ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                         DEVICE VM (Linux + QEMU)                             │
│                                                                              │
│   Receives RPC → Emulates virtio → Accesses virtqueues directly             │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### sDDF is Just Primitives

sDDF at its core is **not** about specific device types. It's a framework providing:

```
┌─────────────────────────────────────────────────────────────────┐
│                    sDDF Core Primitives                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│   1. Shared Memory Regions                                       │
│      └─► Ring buffers, data buffers                             │
│                                                                  │
│   2. Notifications                                               │
│      └─► Async signaling between components                     │
│                                                                  │
│   3. Queue Protocols                                             │
│      └─► Producer/consumer semantics                            │
│                                                                  │
│   These are the SAME primitives as our inter-VM RPC!            │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

Device-specific protocols (network, block) are just **uses** of these primitives:

| "Device" | What it transports | Same primitives? |
|----------|-------------------|------------------|
| sDDF network | Ethernet frames | ✅ Queues + notifications |
| sDDF block | Block I/O requests | ✅ Queues + notifications |
| sDDF serial | Character streams | ✅ Queues + notifications |
| **Our "vm-admin"** | **MMIO ops, IRQs** | ✅ **Queues + notifications** |

**Our inter-VM RPC is just another "device type"** in this sense - a "vm-admin" or "virtio-forwarding" device.

### What This Means for Migration

The migration really **does boil down to replacing CAmkES glue with Microkit equivalents**:

```
┌────────────────────────────────────────────────────────────────────────────┐
│                        Migration Simplification                             │
├────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   KEEP (unchanged):                                                         │
│   ├─ RPC protocol (QEMU_OP_MMIO, QEMU_OP_SET_IRQ, etc.)                   │
│   ├─ Lock-free queue implementation (rpc_queue.h)                          │
│   ├─ io_proxy logic                                                         │
│   └─ Message encoding (mr0, mr1, mr2, mr3)                                 │
│                                                                             │
│   REPLACE (CAmkES → Microkit):                                             │
│   ├─ dataport Buf(4096) vm_iobuf    →  <memory_region name="iobuf"/>       │
│   ├─ dataport Buf(...) vm_memdev    →  <memory_region name="memdev"/>      │
│   ├─ seL4GlobalAsynch notification  →  <channel> element                   │
│   ├─ vm_ntfn_send_emit()            →  microkit_notify(channel)            │
│   ├─ register_async_event_handler() →  notified(channel) callback          │
│   └─ CAmkES templates               →  Simple code or Python generator     │
│                                                                             │
│   The CONTENT stays the same. Only the GLUE changes.                       │
│                                                                             │
└────────────────────────────────────────────────────────────────────────────┘
```

### Concrete Example

**CAmkES (current):**
```camkes
// Shared memory
connection seL4SharedDataWithCaps iobuf(from vm0.vm1_iobuf, to vm1.vm0_iobuf);

// Notifications
connection seL4GlobalAsynch notify(from vm0.vm1_ntfn, to vm1.vm0_ntfn);
```

```c
// C code
void doorbell(void *cookie) {
    vm1_ntfn_send_emit();  // CAmkES-generated
}
```

**Microkit (equivalent):**
```xml
<!-- Shared memory -->
<memory_region name="iobuf" size="0x2000" />
<protection_domain name="vm0">
    <map mr="iobuf" vaddr="0x5000000" perms="rw" />
</protection_domain>
<protection_domain name="vm1">
    <map mr="iobuf" vaddr="0x5000000" perms="rw" />
</protection_domain>

<!-- Notifications -->
<channel>
    <end pd="vm0" id="1" />
    <end pd="vm1" id="1" />
</channel>
```

```c
// C code
void doorbell(void *cookie) {
    microkit_notify(CHANNEL_VM1);  // Microkit API
}

void notified(microkit_channel ch) {
    if (ch == CHANNEL_VM0) {
        rpc_run(&io_proxy);  // Same logic as before!
    }
}
```

**The RPC queue code (`rpc_queue.h`, `io_proxy.c`) remains identical.**

---

## Deep Dive: Challenge Mitigation Strategies

This section provides detailed analysis and mitigation approaches for each major challenge.

---

### Challenge 1: VM Support Maturity (libsel4vm vs libvmm)

#### Current libsel4vm Usage in virtioso-camkes-vm

Analysis of source code reveals these libsel4vm APIs are used:

| API Category | Functions Used | Purpose |
|--------------|----------------|---------|
| **VM Core** | `vm_t`, `vm_vcpu_t` | VM and vCPU data structures |
| **Memory** | `vm_memory_reservation_t`, `vm_ram_register()`, `guest_ram_*` | Guest memory management |
| **Faults** | `memory_fault_callback_fn`, `vm_register_unhandled_mem_fault_callback()` | MMIO fault handling |
| **IRQ** | `vm_inject_irq()`, `guest_irq_controller` | Interrupt injection |
| **Boot** | `vm_register_bootloader_callback()` | Guest bootloader |
| **Cross-VM** | `cross_vm_connections_init()`, `consume_connection_event()` | Shared memory, notifications |

#### libvmm Feature Comparison

Based on [libvmm documentation](https://github.com/au-ts/libvmm):

| Feature | libsel4vm (CAmkES) | libvmm (Microkit) | Gap |
|---------|-------------------|-------------------|-----|
| **VM abstraction** | `vm_t` struct | `virtual_machine` XML element | Different approach |
| **vCPU management** | `vm_vcpu_t`, multi-vCPU | Up to 16 vCPUs | Equivalent |
| **Guest RAM** | `vm_ram_register()` | `<memory_region>` + `<map>` | Equivalent |
| **MMIO faults** | Callback registration | Handled in VMM `protected()` | Different API |
| **GIC emulation** | vGIC driver (v2/v3) | vGIC driver (v2/v3/v4) | **libvmm better** |
| **IRQ injection** | `vm_inject_irq()` | Via libvmm API | Equivalent |
| **Cross-VM connections** | `cross_vm_connection` driver | Microkit channels | Needs new approach |
| **PCI passthrough** | `vpci` driver | Not built-in | **Gap** |
| **virtio** | Via QEMU in Device VM | sDDF integration | Different architecture |

#### Fault Handling: Same Pattern, Different API

A key concern was whether MMIO fault handling differs fundamentally. Analysis shows **the pattern is identical**:

| Aspect | libsel4vm (CAmkES) | libvmm (Microkit) |
|--------|-------------------|-------------------|
| **Registration** | `vm_reserve_memory_at(vm, addr, size, cb, cookie)` | `fault_register_vm_exception_handler(base, end, cb, data)` |
| **Callback** | `memory_fault_result_t fn(vm_t*, vm_vcpu_t*, paddr, len, cookie)` | `bool fn(vcpu_id, offset, fsr, regs*, data)` |
| **Return** | `FAULT_HANDLED` / `FAULT_ERROR` | `true` / `false` |
| **Max handlers** | Dynamic | 16 (sufficient) |

**Both use the same pattern:**
```
1. Register: (address_range, callback, cookie)
2. On fault: find handler → extract fault info → call callback
3. Callback processes (forward to QEMU via RPC)
4. Return success/failure
```

**Our handler adaptation is minimal (~50 lines):**

```c
// Current (libsel4vm)
static memory_fault_result_t mmio_fault_handler(vm_t *vm, vm_vcpu_t *vcpu,
                                                uintptr_t paddr, size_t len,
                                                void *cookie) {
    io_proxy_t *io_proxy = cookie;
    ioreq_start(io_proxy, vcpu->vcpu_id, ..., paddr, len, value);
    return FAULT_HANDLED;
}
vm_reserve_memory_at(vm, ctrl_base, ctrl_size, mmio_fault_handler, io_proxy);

// Adapted (libvmm) - same logic, different signature
static bool mmio_fault_handler(size_t vcpu_id, size_t offset, size_t fsr,
                               seL4_UserContext *regs, void *data) {
    io_proxy_t *io_proxy = data;
    uintptr_t paddr = io_proxy->ctrl_base + offset;
    ioreq_start(io_proxy, vcpu_id, ..., paddr, len, value);  // UNCHANGED
    return true;
}
fault_register_vm_exception_handler(ctrl_base, ctrl_base + ctrl_size,
                                    mmio_fault_handler, io_proxy);
```

**Key insight:** The core logic (`ioreq_start` → RPC → QEMU) remains **identical**. Only the callback wrapper changes.

#### Gap Analysis: What's Missing in libvmm

```
┌────────────────────────────────────────────────────────────────────────┐
│                    Feature Gap Analysis                                 │
├────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ✅ Available in libvmm:                                               │
│     • VM/vCPU management                                                │
│     • Guest memory mapping                                              │
│     • GIC virtualization (v2/v3/v4)                                    │
│     • IRQ injection                                                     │
│     • Basic Linux boot                                                  │
│     • virtio via sDDF                                                   │
│                                                                         │
│  ⚠️ Different approach needed:                                         │
│     • Cross-VM shared memory (use Microkit memory regions)             │
│     • Notifications (use Microkit channels)                             │
│     • MMIO fault handling (different callback mechanism)               │
│                                                                         │
│  ❌ Gap - needs implementation:                                         │
│     • Virtual PCI (vpci) - ECAM, config space emulation                │
│     • PCI device passthrough                                            │
│     • GICv2m MSI frame emulation                                        │
│     • Cross-VM connection PCI device (for Device VM QEMU)              │
│                                                                         │
└────────────────────────────────────────────────────────────────────────┘
```

#### Mitigation Strategy: VM Support

**Option A: Port libsel4vm Components to libvmm**

Port specific components we need:

```
Components to Port:
┌─────────────────────────────────────────────────────────────┐
│  1. vpci.c (Virtual PCI)                                    │
│     • ECAM config space emulation                           │
│     • BAR handling                                          │
│     • Slot assignment                                       │
│     Effort: ~2 weeks                                        │
├─────────────────────────────────────────────────────────────┤
│  2. gicv2m.c (MSI Frame)                                    │
│     • GICv2m register emulation                             │
│     • SPI doorbell mechanism                                │
│     Effort: ~1 week                                         │
├─────────────────────────────────────────────────────────────┤
│  3. cross_vm_connection adaptation                          │
│     • Map to Microkit channels + memory regions             │
│     • PCI device for guest visibility                       │
│     Effort: ~2 weeks                                        │
└─────────────────────────────────────────────────────────────┘
```

**Option B: Contribute to libvmm Upstream**

Advantages:
- Community maintenance
- Broader testing
- Aligns with seL4 ecosystem direction

Process:
1. Open issue on [au-ts/libvmm](https://github.com/au-ts/libvmm)
2. Discuss design approach
3. Submit PR with vpci/MSI support
4. Iterate based on review

**Option C: Hybrid Approach (Recommended)**

1. Start with libvmm base
2. Port vpci/gicv2m as libvmm extensions
3. Upstream after validation
4. Keep tii-specific features in separate layer

```
┌─────────────────────────────────────────────────────────────┐
│                   Recommended Architecture                   │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              tii-sel4-vm-microkit                     │   │
│  │  (TII-specific: io_proxy, RPC queues, templates)     │   │
│  └──────────────────────────────────────────────────────┘   │
│                            │                                 │
│                            ▼                                 │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              tii-libvmm-extensions                    │   │
│  │  (vpci, gicv2m, cross-vm PCI - upstreamable)         │   │
│  └──────────────────────────────────────────────────────┘   │
│                            │                                 │
│                            ▼                                 │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                      libvmm                           │   │
│  │  (upstream: VM, vCPU, memory, GIC, IRQ)              │   │
│  └──────────────────────────────────────────────────────┘   │
│                            │                                 │
│                            ▼                                 │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                   seL4 Microkit                       │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

---

### Challenge 2: Template Elimination

#### Current Template Usage

The project uses 3 CAmkES template files:

| Template | Purpose | Lines | Complexity |
|----------|---------|-------|------------|
| `seL4VirtIODeviceVM.template.c` | Device VM cross-VM setup | ~65 | Low |
| `seL4VirtIODriverVM.template.c` | Driver VM io_proxy instances | ~100 | Medium |
| `pl011.template.c` | PL011 UART emulation | ~30 | Low |

**What Templates Generate:**

```c
// Template input (Jinja2):
/*- for dev in vm_virtio_devices -*/
extern void *vm/*? dev.id ?*/_iobuf;
io_proxy_t vm/*? dev.id ?*/_io_proxy = { ... };
/*- endfor -*/

// Generated output (for 2 devices):
extern void *vm0_iobuf;
io_proxy_t vm0_io_proxy = { ... };
extern void *vm1_iobuf;
io_proxy_t vm1_io_proxy = { ... };
```

#### Mitigation Strategy: Template Elimination

**Option A: Build-Time Code Generation (Python/Shell)**

Replace Jinja2 templates with build scripts:

```python
#!/usr/bin/env python3
# generate_io_proxies.py

import sys
import json

def generate(config):
    devices = config['vm_virtio_devices']

    code = ['#include "io_proxy.h"\n']
    for dev in devices:
        code.append(f'''
extern void *vm{dev['id']}_iobuf;

static uintptr_t vm{dev['id']}_iobuf_get(io_proxy_t *io_proxy) {{
    return (uintptr_t)vm{dev['id']}_iobuf;
}}

static void vm{dev['id']}_notify(void *cookie) {{
    microkit_notify(CHANNEL_VM{dev['id']});
}}

io_proxy_t vm{dev['id']}_io_proxy = {{
    .data_base = {dev['data_base']},
    .data_size = {dev['data_size']},
    .iobuf_get = vm{dev['id']}_iobuf_get,
    .rpc.doorbell = vm{dev['id']}_notify,
}};
''')
    return '\n'.join(code)

if __name__ == '__main__':
    config = json.load(open(sys.argv[1]))
    print(generate(config))
```

**Makefile integration:**

```makefile
generated/io_proxies.c: config.json generate_io_proxies.py
	python3 generate_io_proxies.py $< > $@

vm_driver.elf: generated/io_proxies.c src/main.c
	$(CC) -o $@ $^
```

**Option B: C Preprocessor Macros (Static)**

For simpler cases, use compile-time macros:

```c
// config.h (generated or manual)
#define VM_VIRTIO_DEVICES \
    X(0, 0x50000000, 0x1000000, 0x60000000, 0x2000) \
    X(1, 0x51000000, 0x1000000, 0x61000000, 0x2000)

// io_proxy_instances.c
#define X(id, data_base, data_size, ctrl_base, ctrl_size) \
    extern void *vm##id##_iobuf; \
    io_proxy_t vm##id##_io_proxy = { \
        .data_base = data_base, \
        .data_size = data_size, \
        .iobuf_get = vm##id##_iobuf_get, \
    };
VM_VIRTIO_DEVICES
#undef X
```

**Option C: Static Configuration (No Generation)**

For fixed configurations, just write the code directly:

```c
// For a fixed 2-VM system, no generation needed
io_proxy_t vm0_io_proxy = { ... };
io_proxy_t vm1_io_proxy = { ... };

io_proxy_t *io_proxies[] = { &vm0_io_proxy, &vm1_io_proxy };
```

**Recommended Approach:**

| Configuration Type | Approach |
|-------------------|----------|
| Fixed 2-VM demo | Static code (Option C) |
| Configurable N-VM | Python generator (Option A) |
| Simple parameterization | C macros (Option B) |

```
┌─────────────────────────────────────────────────────────────┐
│               Template Elimination Flow                      │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  CAmkES (Current):                                          │
│                                                              │
│    .camkes ─► CAmkES tools ─► .template.c ─► generated.c    │
│                    │                                         │
│                    ▼                                         │
│              (Python, Jinja2)                                │
│                                                              │
│  Microkit (Proposed):                                        │
│                                                              │
│    config.json ─► generate.py ─► generated.c                │
│         │                                                    │
│         └─► system.xml                                       │
│                                                              │
│  Single source of truth (config.json) for both              │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

---

### Challenge 3: Build System Migration

#### Current Build Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Current Build Stack                       │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  tii_sel4_build/Makefile                                    │
│         │                                                    │
│         ├─► Docker container (build environment)            │
│         │                                                    │
│         ├─► make linux-image (Yocto/Bitbake)               │
│         │         │                                          │
│         │         └─► vm-images/meta-sel4 recipes           │
│         │                                                    │
│         └─► make vm_qemu_virtio (CMake → CAmkES)           │
│                   │                                          │
│                   ├─► CMakeLists.txt                        │
│                   ├─► CAmkES Python tools                   │
│                   ├─► .camkes → .template.c → .c            │
│                   └─► Ninja build                           │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

#### Microkit Build Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                   Microkit Build Stack                       │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  Makefile (or CMake wrapper)                                │
│         │                                                    │
│         ├─► Compile protection domain ELFs                  │
│         │         │                                          │
│         │         ├─► clang/gcc cross-compile               │
│         │         └─► Link with libmicrokit.a               │
│         │                                                    │
│         └─► microkit tool                                   │
│                   │                                          │
│                   ├─► Parse system.xml                      │
│                   ├─► Generate loader                       │
│                   └─► Output: loader.img                    │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

#### Mitigation Strategy: Build System

**Phase 1: Parallel Build System**

Keep existing CAmkES build, add Microkit alongside:

```
tii_sel4_build/
├── Makefile                    # Top-level orchestration
├── camkes/                     # Existing CAmkES build
│   └── CMakeLists.txt
├── microkit/                   # NEW: Microkit build
│   ├── Makefile
│   ├── system.xml
│   └── src/
└── shared/                     # Common code
    ├── io_proxy.c
    └── rpc.c
```

**Phase 2: Unified Configuration**

Single config file generates both:

```python
# config_generator.py
import json
import sys

config = json.load(open('config.json'))

if sys.argv[1] == 'camkes':
    # Generate .camkes and vm.h macros
    generate_camkes(config)
elif sys.argv[1] == 'microkit':
    # Generate system.xml and C headers
    generate_microkit(config)
```

**Phase 3: Yocto Integration**

Microkit recipe for guest images:

```bitbake
# meta-sel4/recipes-kernel/microkit-vmm/microkit-vmm.bb

SUMMARY = "seL4 Microkit VMM"
LICENSE = "BSD-2-Clause"

DEPENDS = "microkit-sdk"

SRC_URI = "git://github.com/tiiuae/tii-sel4-vm-microkit.git;branch=main"

do_compile() {
    oe_runmake MICROKIT_SDK=${STAGING_DIR_HOST}/opt/microkit
}

do_install() {
    install -m 0644 ${B}/loader.img ${D}/boot/
}
```

**Phase 4: CI/CD Pipeline**

```yaml
# .github/workflows/build-microkit.yml
name: Build Microkit VMM

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Setup Microkit SDK
        run: |
          wget https://github.com/seL4/microkit/releases/download/2.0.1/microkit-sdk-2.0.1-linux-x86_64.tar.gz
          tar xzf microkit-sdk-*.tar.gz

      - name: Build VMM
        run: make MICROKIT_SDK=$PWD/microkit-sdk-2.0.1

      - name: Upload artifacts
        uses: actions/upload-artifact@v4
        with:
          name: loader.img
          path: build/loader.img
```

---

## Implementation Roadmap

Based on the challenge analysis, here's a refined implementation plan:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        Implementation Roadmap                            │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Month 1: Foundation & Proof of Concept                                 │
│  ├─ Week 1-2: Minimal Microkit prototype (ping-pong VMs)               │
│  │            • Two protection domains with shared memory               │
│  │            • Bidirectional notifications                             │
│  │            • Validates Microkit can express our pattern             │
│  │                                                                       │
│  ├─ Week 3: Cross-VM PCI minimal implementation (Option B)             │
│  │            • ~200-300 lines of cross_vm_pci.c                        │
│  │            • Config space stub for device discovery                  │
│  │            • BAR fault handlers (doorbell + shared memory)          │
│  │            • Guest driver (kmod-sel4-virt) unchanged                │
│  │                                                                       │
│  └─ Week 4: RPC queue integration                                       │
│              • Verify rpc_queue.h works unchanged                       │
│              • Connect io_proxy with Microkit glue                      │
│              • End-to-end MMIO forwarding test                          │
│                                                                          │
│  Month 2: QEMU Integration                                               │
│  ├─ Week 1: Template elimination                                        │
│  │            • Python generator for multi-VM configs                   │
│  │            • Or static code for fixed configurations                │
│  │                                                                       │
│  ├─ Week 2: Build system parallel setup                                 │
│  │            • Microkit build alongside CAmkES                         │
│  │            • Shared source code where possible                       │
│  │                                                                       │
│  └─ Week 3-4: QEMU in Device VM                                        │
│                • Full virtio backend operation                          │
│                • Guest Linux boots with virtio devices                  │
│                                                                          │
│  Month 3: Validation & Polish                                            │
│  ├─ Week 1-2: Functional testing                                        │
│  │            • virtio-net: ping, iperf                                 │
│  │            • virtio-blk: fio, filesystem operations                  │
│  │            • virtio-console: serial I/O                              │
│  │                                                                       │
│  ├─ Week 3: Performance comparison                                      │
│  │            • CAmkES vs Microkit latency/throughput                   │
│  │            • Identify any regressions                                │
│  │                                                                       │
│  └─ Week 4: CI/CD, documentation                                        │
│              • GitHub Actions workflow                                   │
│              • Migration guide                                           │
│                                                                          │
│  Optional Month 4: Full PCI Passthrough                                  │
│  ├─ Week 1-2: Port vpci.c to libvmm                                     │
│  │            • Full ECAM emulation                                      │
│  │            • Multiple PCI devices                                     │
│  │                                                                       │
│  └─ Week 3-4: Port gicv2m MSI emulation                                │
│                • SPI doorbell mechanism                                  │
│                • MSI interrupt support                                   │
│                • Consider upstream contribution                          │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

---

### Challenge 4: Cross-VM PCI Device for Guest

The Device VM needs to see a PCI device that provides access to shared memory and notification mechanisms. This is how the guest Linux (running QEMU) connects to the seL4 VMM.

#### Current Implementation Analysis

**VMM Side (`cross_vm_connection.c` in libsel4vmmplatsupport):**

```c
// PCI device with TII-specific vendor/device ID
vmm_pci_device_def_t pci_config = {
    .vendor_id = 0x1af4,           // Red Hat/QEMU
    .device_id = 0xa111,           // TII custom (NOT a VirtIO device)
    .class_code = PCI_CLASS_MEMORY_RAM,
    .bar0 = event_address | PCI_BASE_ADDRESS_SPACE_MEMORY,   // Doorbell registers
    .bar1 = dataport_address | PCI_BASE_ADDRESS_SPACE_MEMORY, // Shared memory
};
```

**Guest Side (`sel4_pci.c` in kmod-sel4-virt):**

```c
// Standard Linux PCI driver
static struct pci_device_id sel4_pci_ids[] = {
    { .vendor = PCI_VENDOR_ID_REDHAT_QUMRANET, .device = 0xa111 },
    { 0 }
};

// Maps BARs, initializes RPC queues
void *iobuf = ioremap_cache(pci_resource_start(dev, 1), size);
vso_rpc_init(&vmm->rpc, vso_rpc_device_km, iobuf, doorbell_cb, cookie);
```

**Key insight: This is NOT a VirtIO device.** It's a custom PCI device with:
- Vendor 0x1af4 (Red Hat/QEMU range)
- Device 0xa111 (TII-allocated ID)
- No VirtIO capabilities - just two memory BARs

#### libvmm virtio-pci Analysis

**What libvmm provides (`src/virtio/pci.c`):**

| Feature | libvmm virtio-pci | TII needs |
|---------|-------------------|-----------|
| **ECAM config space** | ✅ Full emulation | ✅ Basic config space |
| **BAR allocation** | ✅ Via `virtio_pci_alloc_memory_bar()` | ✅ Two fixed BARs |
| **Fault handling** | ✅ `virtio_ecam_fault_handle()` | ✅ BAR fault handlers |
| **Device types** | ❌ VirtIO only | ❌ Custom device |
| **Capability parsing** | ❌ VirtIO-specific | ❌ Not needed |
| **Generic callbacks** | ❌ Hardcoded VirtIO dispatch | ❌ Custom handlers |

**Critical finding:** libvmm's virtio-pci **cannot be used directly** because:

1. All config space handlers dispatch through VirtIO-specific functions
2. Device registration requires VirtIO capability structures
3. BAR fault handlers assume VirtIO capability types
4. No support for arbitrary custom PCI devices

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    libvmm virtio-pci Architecture                        │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│   Config Space Fault                                                     │
│         │                                                                │
│         ▼                                                                │
│   virtio_ecam_fault_handle()                                            │
│         │                                                                │
│         ├─► Parse VirtIO capability type                                │
│         │                                                                │
│         ├─► Dispatch to:                                                 │
│         │     • handle_virtio_common_read/write()  ─┐                   │
│         │     • handle_virtio_device_read/write()  ─┼─► VirtIO-specific │
│         │     • handle_virtio_notify_write()       ─┘                   │
│         │                                                                │
│         └─► No generic callback mechanism                               │
│                                                                          │
│   ❌ Cannot add non-VirtIO device without major modifications           │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

#### Migration Options for Cross-VM PCI

**Option A: Port TII vpci + cross_vm_connection (Recommended)**

Port the existing vpci from libsel4vmmplatsupport to work with libvmm:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    Option A: Port vpci Architecture                      │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Files to port:                                                          │
│                                                                          │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │  vpci.c (~400 lines)                                             │    │
│  │  • ECAM config space fault handling                              │    │
│  │  • PCI slot management (vmm_pci_space_t)                        │    │
│  │  • BAR emulation with generic callbacks                          │    │
│  │  • FDT node generation (pci@ node)                               │    │
│  │                                                                   │    │
│  │  Adaptation needed:                                               │    │
│  │  • vm_reserve_memory_at() → fault_register_vm_exception_handler()│    │
│  │  • vm_inject_irq() → libvmm IRQ injection                        │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │  cross_vm_connection.c (~300 lines)                              │    │
│  │  • Create PCI device with 2 BARs                                 │    │
│  │  • BAR0: Event registers (doorbell)                              │    │
│  │  • BAR1: Dataport (shared memory)                                │    │
│  │  • IRQ on notification                                           │    │
│  │                                                                   │    │
│  │  Adaptation needed:                                               │    │
│  │  • CAmkES emit_fn → microkit_notify()                            │    │
│  │  • consume_id matching → Microkit channel ID                     │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  Effort: ~3-4 weeks                                                      │
│  Risk: Low (proven code, adaptation only)                                │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

**Option B: Minimal Cross-VM PCI Emulation (Faster)**

Create purpose-built minimal PCI emulation for just cross-VM device:

```c
// Minimal cross-VM PCI - no full vpci needed
// ~200-300 lines of new code

struct cross_vm_pci {
    uintptr_t config_base;      // Where guest sees PCI config space
    uintptr_t event_bar_base;   // BAR0: doorbell registers
    uintptr_t dataport_base;    // BAR1: shared memory
    size_t    dataport_size;
    int       virq;             // Virtual IRQ for notifications
    microkit_channel notify_ch; // Channel to peer VM
};

// Config space handler - minimal, just enough for device discovery
static bool cross_vm_config_fault(size_t vcpu, size_t offset, size_t fsr,
                                   seL4_UserContext *regs, void *data) {
    struct cross_vm_pci *pci = data;

    // Return fixed config for our device:
    // vendor=0x1af4, device=0xa111, class=memory, 2 BARs
    // Guest will see this via lspci and bind driver
}

// BAR0 (event bar) fault handler - doorbell mechanism
static bool event_bar_fault(size_t vcpu, size_t offset, size_t fsr,
                            seL4_UserContext *regs, void *data) {
    struct cross_vm_pci *pci = data;

    if (is_write && offset == EVENT_BAR_EMIT_REGISTER) {
        microkit_notify(pci->notify_ch);  // Ring doorbell
    }
    // Handle consume register, etc.
}

// BAR1 (dataport) - direct mapping, no faults normally
// (shared memory mapped at init time)
```

**Advantages:**
- Faster implementation (~1 week)
- No vpci dependency
- Minimal code to maintain

**Disadvantages:**
- Not reusable for other PCI devices
- Duplicates some vpci functionality

**Option C: Contribute Generic PCI Layer to libvmm**

Extract the non-VirtIO parts from libvmm's pci.c, make it generic:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    Option C: Upstream Contribution                       │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Proposed libvmm changes:                                               │
│                                                                          │
│  1. Split virtio/pci.c into:                                            │
│     ├─ pci/ecam.c      (generic ECAM handling)                          │
│     ├─ pci/device.c    (generic device registration)                    │
│     └─ virtio/pci.c    (VirtIO-specific, uses pci/*)                   │
│                                                                          │
│  2. Add generic BAR callback interface:                                 │
│     typedef bool (*pci_bar_handler_t)(size_t bar, size_t offset,        │
│                                       bool is_write, uint32_t *value,   │
│                                       void *data);                      │
│                                                                          │
│  3. Device registration with custom handlers:                           │
│     pci_register_device(slot, config, bar_handlers, data);             │
│                                                                          │
│  Benefits:                                                               │
│  • Community maintenance                                                 │
│  • Enables other non-VirtIO PCI uses                                    │
│  • Aligns with libvmm architecture                                      │
│                                                                          │
│  Challenges:                                                             │
│  • Upstream review/acceptance time                                      │
│  • May require design discussions                                       │
│  • Longer timeline                                                       │
│                                                                          │
│  Effort: 4-6 weeks (including upstream coordination)                    │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

#### Recommendation: Cross-VM PCI

**For Initial Migration:** Use **Option B (Minimal Cross-VM PCI)** for fastest results:

1. Create standalone `cross_vm_pci.c` (~200-300 lines)
2. Register fault handlers with libvmm's fault API
3. Keep guest driver (kmod-sel4-virt) unchanged
4. Works with existing QEMU/vso_rpc

**For Long-Term:** Pursue **Option C** as follow-up:

1. After migration validated, contribute generic PCI to libvmm
2. Benefits whole community
3. TII becomes libvmm contributor

```
┌─────────────────────────────────────────────────────────────────────────┐
│               Cross-VM PCI Migration Summary                             │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  KEEP UNCHANGED:                                                         │
│  ├─ Guest driver (kmod-sel4-virt/sel4_pci.c)                           │
│  ├─ PCI device ID (0x1af4:0xa111)                                       │
│  ├─ BAR layout (event + dataport)                                       │
│  └─ RPC queue protocol                                                   │
│                                                                          │
│  REPLACE:                                                                │
│  ├─ vmm_pci_add_entry() → custom fault registration                     │
│  ├─ create_allocated_reservation_frame() → libvmm fault handler         │
│  ├─ CAmkES emit_fn → microkit_notify()                                  │
│  └─ vm_inject_irq() → libvmm virq_inject()                              │
│                                                                          │
│  NEW CODE REQUIRED:                                                      │
│  └─ cross_vm_pci.c (~200-300 lines)                                     │
│      • PCI config space stub (device discovery)                          │
│      • BAR0 fault handler (doorbell)                                     │
│      • BAR1 mapping (shared memory)                                      │
│      • IRQ injection on notification                                     │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Resources

- [seL4 Microkit Documentation](https://github.com/seL4/microkit)
- [Microkit Tutorial](https://trustworthy.systems/projects/microkit/tutorial/)
- [libvmm Repository](https://github.com/au-ts/libvmm)
- [libvmm Manual](https://github.com/au-ts/libvmm/blob/main/docs/MANUAL.md)
- [CAmkES Manual](https://docs.sel4.systems/projects/camkes/)
- [seL4 Discourse - Microkit discussions](https://sel4.discourse.group/)

## Appendix: Feature Mapping Reference

### CAmkES Connection Types → Microkit

| CAmkES | Microkit | Notes |
|--------|----------|-------|
| `seL4SharedData` | `<memory_region>` + `<map>` | Explicit in XML |
| `seL4SharedDataWithCaps` | `<memory_region>` + `<map>` | Caps handled differently |
| `seL4Notification` | `<channel>` | Bidirectional by default |
| `seL4GlobalAsynch` | `<channel>` | Same mechanism |
| `seL4RPCCall` | Manual IPC | `microkit_ppcall()` |
| `seL4VMDTBPassthrough` | Custom | No direct equivalent |

### CAmkES Runtime → Microkit

| CAmkES | Microkit | Notes |
|--------|----------|-------|
| `camkes.h` | `microkit.h` | Different API |
| `seL4_*_emit()` | `microkit_notify()` | Channel-based |
| `register_async_event_handler()` | `notified()` | Callback vs switch |
| Pre-/post-init hooks | `init()` | Single init function |
| Configuration attributes | XML + headers | Build-time config |

## Related Documentation

- [System Overview](../architecture/overview.md) - Current CAmkES architecture
- [CAmkES Templates](../components/camkes-templates.md) - Template system details
- [Build Architecture](../build-system/build-architecture.md) - Current build system
