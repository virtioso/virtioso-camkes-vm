# GICv3 Virtual GIC Implementation Plan

## Overview

The seL4 VMM currently only supports GICv2 virtual interrupt controller emulation. Platforms with GICv3 (like NVIDIA Orin AGX/Tegra234) cannot run guest VMs until GICv3 vGIC support is implemented.

### Related Documents

| Document | Purpose |
|----------|---------|
| [vm-qemu-virtio-orinagx.md](../porting/vm-qemu-virtio-orinagx.md) | Current CAmkES VM porting notes for Orin AGX |
| `kernel/tools/dts/orinagx.dts` | Platform device tree with GIC addresses |
| `projects/vm/components/VM_Arm/plat_include/orinagx/plat/vmlinux.h` | Guest DT node filtering |

### Integration with CAmkES VM

The GICv3 vGIC implementation is in **libsel4vm** (`projects/sel4_projects_libs/libsel4vm/`). CAmkES VM applications (like vm_minimal) call `vm_create_default_irq_controller()` which invokes `vm_install_vgic()`.

**No changes required to CAmkES VM applications** - the API remains the same. Only libsel4vm internals change:
- New `vgic_v3.c` replaces `vgic_v2.c` for GICv3 platforms
- Build system selects v2 or v3 based on `KernelArmGicV3`

## Current GICv2 Implementation

Location: `projects/sel4_projects_libs/libsel4vm/src/arch/arm/vgic/`

| File | Purpose |
|------|---------|
| `vgic_v2.c` | Main GICv2 emulation logic |
| `gicv2.h` | Platform-specific GIC addresses |
| `vgicv2_defs.h` | GICv2 register definitions |
| `vdist.h` | Virtual distributor state |
| `virq.h` | Virtual IRQ management |
| `vgic.h` | Public vGIC interface |

### GICv2 Architecture (current)

```
┌─────────────────────────────────────────────┐
│              GIC Distributor (GICD)          │
│              Single shared block             │
│         Routes interrupts to CPUs            │
└─────────────────┬───────────────────────────┘
                  │
    ┌─────────────┼─────────────┐
    │             │             │
┌───▼───┐    ┌───▼───┐    ┌───▼───┐
│ GICC  │    │ GICC  │    │ GICC  │
│ CPU 0 │    │ CPU 1 │    │ CPU N │
└───────┘    └───────┘    └───────┘
  CPU Interface (per-CPU, memory-mapped)
```

## GICv3 Architecture (needed)

```
┌─────────────────────────────────────────────┐
│              GIC Distributor (GICD)          │
│              SPIs (shared interrupts)        │
└─────────────────┬───────────────────────────┘
                  │
    ┌─────────────┼─────────────┐
    │             │             │
┌───▼───┐    ┌───▼───┐    ┌───▼───┐
│ GICR  │    │ GICR  │    │ GICR  │
│ CPU 0 │    │ CPU 1 │    │ CPU N │
└───┬───┘    └───┬───┘    └───┬───┘
    │  Redistributor (per-CPU, memory-mapped)
    │  Handles SGIs, PPIs, LPIs
    │
┌───▼───────────────────────────────┐
│     CPU Interface (System Regs)    │
│     ICC_* registers (not MMIO)     │
└───────────────────────────────────┘

┌─────────────────────────────────────────────┐
│     ITS (Interrupt Translation Service)      │
│     Optional - for MSI/MSI-X support         │
└─────────────────────────────────────────────┘
```

## Key Differences: GICv2 vs GICv3

| Feature | GICv2 | GICv3 |
|---------|-------|-------|
| CPU Interface | Memory-mapped (GICC) | System registers (ICC_*) |
| Per-CPU config | Via GICD | Via GICR (Redistributor) |
| Max CPUs | 8 | 2^16 (affinity routing) |
| Max SPIs | 1020 | 1020 |
| LPIs | Not supported | Supported (for MSI) |
| Security | 2 states | 2 states + EL2 |
| Affinity | Linear CPU ID | Hierarchical (Aff3.Aff2.Aff1.Aff0) |

## Implementation Requirements

### 1. New Source Files

```
libsel4vm/src/arch/arm/vgic/
├── vgic_v3.c          # GICv3 emulation logic (NEW)
├── vgicv3_defs.h      # GICv3 register definitions (NEW)
├── gicv3.h            # Platform GICv3 addresses (NEW)
├── vgicr.h            # Virtual redistributor state (NEW)
└── vgic.c             # Updated to select v2/v3
```

### 2. GICD (Distributor) Changes

GICv3 GICD is similar to GICv2 but with extensions:

| Register | Offset | GICv2 | GICv3 | Notes |
|----------|--------|-------|-------|-------|
| GICD_CTLR | 0x0000 | ✓ | Modified | New bits for affinity routing |
| GICD_TYPER | 0x0004 | ✓ | Modified | Reports GICv3 features |
| GICD_IIDR | 0x0008 | ✓ | ✓ | |
| GICD_IGROUPR | 0x0080 | ✓ | ✓ | |
| GICD_ISENABLER | 0x0100 | ✓ | ✓ | |
| GICD_ICENABLER | 0x0180 | ✓ | ✓ | |
| GICD_ISPENDR | 0x0200 | ✓ | ✓ | |
| GICD_ICPENDR | 0x0280 | ✓ | ✓ | |
| GICD_ISACTIVER | 0x0300 | ✓ | ✓ | |
| GICD_ICACTIVER | 0x0380 | ✓ | ✓ | |
| GICD_IPRIORITYR | 0x0400 | ✓ | ✓ | |
| GICD_ITARGETSR | 0x0800 | ✓ | Removed | Use IROUTER instead |
| GICD_ICFGR | 0x0C00 | ✓ | ✓ | |
| GICD_IGRPMODR | 0x0D00 | N/A | New | Group modifier |
| GICD_IROUTER | 0x6000 | N/A | New | Affinity routing |

**Key GICD changes for GICv3**:
- `GICD_CTLR.ARE_S` and `GICD_CTLR.ARE_NS` enable affinity routing
- `GICD_ITARGETSR` replaced by `GICD_IROUTER` (64-bit per SPI)
- New `GICD_IGRPMODR` for group modifier

### 3. GICR (Redistributor) - NEW

Each CPU has a Redistributor with two 64KB frames:

**RD_base (frame 0)** - Control and LPI config:
| Register | Offset | Purpose |
|----------|--------|---------|
| GICR_CTLR | 0x0000 | Redistributor control |
| GICR_IIDR | 0x0004 | Implementer ID |
| GICR_TYPER | 0x0008 | Type (64-bit, has CPU affinity) |
| GICR_WAKER | 0x0014 | Wake control |
| GICR_PROPBASER | 0x0070 | LPI config table base |
| GICR_PENDBASER | 0x0078 | LPI pending table base |

**SGI_base (frame 1)** - SGI/PPI config (like GICD for private IRQs):
| Register | Offset | Purpose |
|----------|--------|---------|
| GICR_IGROUPR0 | 0x0080 | Group for SGI/PPI |
| GICR_ISENABLER0 | 0x0100 | Enable SGI/PPI |
| GICR_ICENABLER0 | 0x0180 | Disable SGI/PPI |
| GICR_ISPENDR0 | 0x0200 | Set pending |
| GICR_ICPENDR0 | 0x0280 | Clear pending |
| GICR_ISACTIVER0 | 0x0300 | Set active |
| GICR_ICACTIVER0 | 0x0380 | Clear active |
| GICR_IPRIORITYR | 0x0400 | Priority (8 regs) |
| GICR_ICFGR0/1 | 0x0C00 | Configuration |

### 4. CPU Interface (System Registers) - MAJOR CHANGE

GICv3 CPU interface uses **system registers**, not memory-mapped I/O:

| Register | Op0 | Op1 | CRn | CRm | Op2 | Purpose |
|----------|-----|-----|-----|-----|-----|---------|
| ICC_IAR0_EL1 | 3 | 0 | 12 | 8 | 0 | Interrupt Acknowledge (Grp0) |
| ICC_IAR1_EL1 | 3 | 0 | 12 | 12 | 0 | Interrupt Acknowledge (Grp1) |
| ICC_EOIR0_EL1 | 3 | 0 | 12 | 8 | 1 | End of Interrupt (Grp0) |
| ICC_EOIR1_EL1 | 3 | 0 | 12 | 12 | 1 | End of Interrupt (Grp1) |
| ICC_HPPIR0_EL1 | 3 | 0 | 12 | 8 | 2 | Highest Pending (Grp0) |
| ICC_HPPIR1_EL1 | 3 | 0 | 12 | 12 | 2 | Highest Pending (Grp1) |
| ICC_BPR0_EL1 | 3 | 0 | 12 | 8 | 3 | Binary Point (Grp0) |
| ICC_BPR1_EL1 | 3 | 0 | 12 | 12 | 3 | Binary Point (Grp1) |
| ICC_PMR_EL1 | 3 | 0 | 4 | 6 | 0 | Priority Mask |
| ICC_CTLR_EL1 | 3 | 0 | 12 | 12 | 4 | Control |
| ICC_SRE_EL1 | 3 | 0 | 12 | 12 | 5 | System Register Enable |
| ICC_IGRPEN0_EL1 | 3 | 0 | 12 | 12 | 6 | Group 0 Enable |
| ICC_IGRPEN1_EL1 | 3 | 0 | 12 | 12 | 7 | Group 1 Enable |
| ICC_SGI0R_EL1 | 3 | 0 | 12 | 11 | 7 | SGI Generation (Grp0) |
| ICC_SGI1R_EL1 | 3 | 0 | 12 | 11 | 5 | SGI Generation (Grp1) |

**Implementation approach**: Trap ICC_* register accesses in the hypervisor and emulate them.

### 5. Hypervisor Interface (ICH_* registers)

For virtualizing GICv3, the hypervisor uses ICH_* system registers:

| Register | Purpose |
|----------|---------|
| ICH_HCR_EL2 | Hypervisor control |
| ICH_VTR_EL2 | VGIC type (read-only) |
| ICH_MISR_EL2 | Maintenance interrupt status |
| ICH_VMCR_EL2 | Virtual machine control |
| ICH_LR<n>_EL2 | List registers (virtual IRQ injection) |
| ICH_AP0R<n>_EL2 | Active priorities (Grp0) |
| ICH_AP1R<n>_EL2 | Active priorities (Grp1) |

**Key**: `ICH_LR<n>_EL2` list registers are used to inject virtual interrupts to the guest, similar to GICv2's `GICH_LR<n>`.

### 6. Orin AGX Specific Values

From `kernel/tools/dts/orinagx.dts`:

```c
#define ORINAGX_GICD_PADDR      0x0F400000  /* Distributor */
#define ORINAGX_GICD_SIZE       0x10000     /* 64KB */
#define ORINAGX_GICR_PADDR      0x0F440000  /* Redistributors */
#define ORINAGX_GICR_SIZE       0x200000    /* 2MB (for all CPUs) */
#define ORINAGX_GICR_STRIDE     0x20000     /* 128KB per CPU (2 frames) */
```

### 7. Implementation Steps

#### Phase 1: Basic GICv3 Structure
1. Create `vgicv3_defs.h` with register definitions
2. Create `gicv3.h` with platform addresses
3. Create `vgicr.h` for redistributor state
4. Add build system support to select GICv2 vs GICv3

#### Phase 2: GICD Emulation
1. Update GICD emulation for GICv3 differences
2. Implement `GICD_IROUTER` instead of `GICD_ITARGETSR`
3. Handle `ARE` (Affinity Routing Enable) bit

#### Phase 3: GICR Emulation
1. Implement per-CPU redistributor state
2. Handle RD_base frame (GICR_CTLR, GICR_TYPER, GICR_WAKER)
3. Handle SGI_base frame (SGI/PPI enable, pending, priority)
4. Memory map GICR regions to guest

#### Phase 4: CPU Interface (System Registers)
1. Trap ICC_* register accesses (requires kernel support)
2. Emulate ICC_IAR, ICC_EOIR for interrupt acknowledge/EOI
3. Emulate ICC_PMR, ICC_BPR for priority
4. Emulate ICC_SGI*R for SGI generation

#### Phase 5: Virtual Interrupt Injection
1. Use ICH_LR<n>_EL2 for injecting interrupts
2. Handle maintenance interrupts (ICH_MISR_EL2)
3. Manage list register allocation

#### Phase 6: LPI Support (Optional, for MSI)
1. Implement GICR_PROPBASER/PENDBASER handling
2. Create LPI configuration table
3. Handle ITS commands (if ITS virtualization needed)

### 8. Kernel Changes Required

#### 8.1 Good News: Much Already Works!

The seL4 kernel already has significant GICv3 support:

**What's already implemented:**
- `ICH_LRn_EL2` list register access (`gic_v3.h:440-550`) for virtual IRQ injection
- `ICH_HCR_EL2`, `ICH_VMCR_EL2`, etc. for vGIC control
- VCPU fault forwarding to userspace VMM (`seL4_Fault_VCPUFault` with full ESR)
- System register traps are forwarded via `c_handle_vcpu_fault` → VMM receives HSR/ESR

**Exception trap flow (already working):**
```
Guest: MRS x0, ICC_IAR1_EL1  (if trapping enabled)
   ↓
EL2: lower_el_sync → ESR_EL2 has EC=0x18 (MSR/MRS trap)
   ↓
Kernel: c_handle_vcpu_fault(esr)
   ↓
VMM: seL4_Fault_VCPUFault with seL4_VCPUFault_HSR containing full ESR
```

The ESR contains:
- `EC` (bits 31:26) = 0x18 for MSR/MRS traps
- `Op0/Op1/CRn/CRm/Op2` (bits 20:14, 13:10, 9:5, 4:1) = identifies which ICC_* register
- `Rt` (bits 9:5) = which Xn register
- `Direction` (bit 0) = read (MRS) vs write (MSR)

#### 8.2 What's Missing: ICC_SRE_EL2 Configuration

**The kernel never configures `ICC_SRE_EL2`!**

Current kernel code (`gic_v3.c:138-147`):
```c
static void gicv3_enable_sre(void)
{
    word_t val = 0;
    /* ICC_SRE_EL1 */
    SYSTEM_READ_WORD(ICC_SRE_EL1, val);
    val |= GICC_SRE_EL1_SRE;
    SYSTEM_WRITE_WORD(ICC_SRE_EL1, val);
    isb();
}
```

Only `ICC_SRE_EL1.SRE=1` is set. `ICC_SRE_EL2` is left at reset value (IMPLEMENTATION DEFINED).

**To trap guest ICC_* accesses, the kernel needs to:**
```c
// In vcpu_boot_init() or armv_vcpu_boot_init():
word_t sre_el2 = 0;
SYSTEM_READ_WORD(ICC_SRE_EL2, sre_el2);
sre_el2 &= ~GICC_SRE_EL2_ENABLE;  // Clear Enable bit to trap EL1 accesses
SYSTEM_WRITE_WORD(ICC_SRE_EL2, sre_el2);
isb();
```

Or alternatively, set `ICC_SRE_EL2.SRE=0` to disable system register interface at EL1.

#### 8.3 Summary of Required Kernel Changes

| Change | Location | Complexity |
|--------|----------|------------|
| Configure ICC_SRE_EL2 to trap guest ICC_* | `armv_vcpu_boot_init()` | Low (~10 lines) |
| Define ICC_SRE_EL2 register encoding | `gic_v3.h` | Low (~5 lines) |

**No changes needed for:**
- ICH_LR injection (already works)
- VCPU fault forwarding (already works)
- ESR decoding in VMM (ESR already forwarded)

Check `kernel/src/arch/arm/machine/gic_v3.c` for existing GICv3 support.

### 9. References

- ARM GICv3/v4 Architecture Specification (IHI 0069)
- ARM GICv3 Software Overview (DEN 0024)
- Linux KVM vGICv3 implementation: `arch/arm64/kvm/vgic/vgic-v3.c`
- Xen vGICv3: `xen/arch/arm/vgic-v3.c`

### 10. Estimated Effort (Revised)

Based on code analysis, kernel changes are simpler than initially estimated:

| Component | Complexity | Notes |
|-----------|------------|-------|
| Kernel: ICC_SRE_EL2 config | **Low** | ~15 lines in `armv_vcpu_boot_init()` |
| GICD v3 updates | Medium | Mostly compatible with v2 |
| GICR emulation | High | New component, ~1000 LOC |
| ICC_* emulation in VMM | Medium | ESR already forwarded, decode + handle |
| ICH_* integration | **Already done** | Kernel uses ICH_LRn_EL2 |
| Testing/debugging | High | Complex virtualization interactions |

**Revised estimates:**
| Phase | Scope |
|-------|-------|
| Phase 1: Kernel patch | Enable ICC_* trapping |
| Phase 2: VMM ICC_* handler | Decode ESR, emulate registers |
| Phase 3: GICR emulation | Per-CPU redistributor |
| Phase 4: GICD updates | Affinity routing, IROUTER |
| Phase 5: Integration | Test with Linux guest |

### 11. Alternative Approaches

1. **GICv2 Compatibility Mode**: Some GICv3 implementations support GICv2 compatibility. Check if Tegra234 supports this (unlikely for full VM use).

2. **Paravirtualized Interrupts**: Instead of emulating GIC, use paravirtualized interrupt delivery. Requires guest kernel modifications.

3. **Direct IRQ Injection**: For simple cases, bypass full GIC emulation and directly inject interrupts. Limited functionality.

## 12. Detailed Implementation Findings

### 12.1 Guest Register Write-back (for MRS/MSR Emulation)

When emulating ICC_* system register accesses, VMM needs to read/write guest general registers:

**Location**: `libsel4vm/src/sel4_arch/aarch64/fault.c:13-85`

```c
// Get pointer to guest register Xn from fault context
seL4_Word *decode_rt(int reg, seL4_UserContext *c)
{
    switch (reg) {
    case  0: return &c->x0;
    case  1: return &c->x1;
    // ... up to x30
    case 31: return &wzr;  // zero register (reads 0, writes discard)
    }
}

// Usage pattern for MRS emulation:
seL4_UserContext *ctx = fault_get_ctx(fault);  // Calls seL4_TCB_ReadRegisters
int rt = get_rt(fault);                        // Extract Xt from ESR[9:5]
seL4_Word *reg_ctx = decode_rt(rt, ctx);
*reg_ctx = emulated_value;                     // Write return value
ignore_fault(fault);                           // Calls seL4_TCB_WriteRegisters
```

**For AArch64**: `decode_vcpu_reg()` always returns `seL4_VCPUReg_Num` (no register banking).

### 12.2 MMIO Fault Handler Registration

**API**: `vm_reserve_memory_at()` from `libsel4vm/include/sel4vm/guest_memory.h`

```c
// Register MMIO fault handler for a memory region
vm_memory_reservation_t *vm_reserve_memory_at(
    vm_t *vm,
    uintptr_t addr,      // Guest physical address
    size_t size,         // Region size
    memory_fault_callback_fn fault_callback,
    void *cookie         // Passed to callback
);

// Callback signature
typedef memory_fault_result_t (*memory_fault_callback_fn)(
    vm_t *vm,
    vm_vcpu_t *vcpu,
    uintptr_t fault_addr,
    size_t fault_length,
    void *cookie
);

// Return values
typedef enum {
    FAULT_HANDLED,    // Fault handled, continue guest
    FAULT_UNHANDLED,  // Not handled, try next handler
    FAULT_ERROR       // Error, abort
} memory_fault_result_t;
```

**Example from vgic_v2.c**:
```c
vm_memory_reservation_t *vgic_dist_res = vm_reserve_memory_at(
    vm,
    GIC_DIST_PADDR,          // 0x08000000 for qemu-arm-virt
    PAGE_SIZE_4K,
    handle_vgic_dist_fault,
    (void *)vgic_dist
);
```

### 12.3 virq_t Format for GICv3

**VMM side** (`virq.h:33-38`):
```c
struct virq_handle {
    int virq;            // Virtual IRQ number (0-1019)
    int level;           // Current level (for level-triggered)
    irq_ack_fn_t ack;    // Callback when guest EOIs
    void *token;         // Opaque callback data
};
```

**Kernel side** (`structures.bf:353-390` for GICv3):
```
block virq_pending {
    field virqType      2     // bits 0-1:  0=invalid, 1=pending, 2=active
    padding             1     // bit 2
    field virqGroup     1     // bit 3:     interrupt group (0 or 1)
    padding             4     // bits 4-7
    field virqPriority  8     // bits 8-15: priority (0=highest)
    padding             6     // bits 16-21
    field virqEOIIRQEN  1     // bit 22:    EOI IRQ enable
    padding             9     // bits 23-31
    field virqIRQ       32    // bits 32-63: virtual INTID
}
```

**Injection flow**:
```
VMM: seL4_ARM_VCPU_InjectIRQ(vcpu_cap, virq, priority, group, lr_idx)
  ↓
Kernel: invokeVCPUInjectIRQ() creates virq_t from parameters
  ↓
Kernel: set_gic_vcpu_ctrl_lr(idx, virq) writes to ICH_LRn_EL2
```

### 12.4 vGIC State Machine

**States** (documented in vgic_v2.c:7-35):
```
b) ENABLING: Guest enables IRQ in GICD
   - If not pending: ACK with seL4
   - If pending: no action

c) PIRQ: Physical IRQ received from seL4
   - If IRQ enabled: set pending, inject to guest → state d
   - If disabled: ignore → state b

d) GUEST ACK: Guest acknowledges IRQ (reads ICC_IAR)
   - Hardware transitions LR: pending → active
   - No VMM involvement

e) GUEST EOI: Guest writes ICC_EOIR
   - Hardware clears LR, triggers maintenance interrupt
   - Kernel sends seL4_Fault_VGICMaintenance to VMM
   - VMM: clear pending, call ack callback, dequeue next IRQ

g) DISABLE: Guest disables IRQ
   - Allow in-flight IRQ to complete
   - Block future injections
```

**Key data structures**:
```c
// Per-VCPU interrupt context
typedef struct vgic_vcpu {
    virq_handle_t lr_shadow[NUM_LIST_REGS];  // Mirrors ICH_LRn_EL2
    struct irq_queue irq_queue;               // Overflow when LRs full
    virq_handle_t local_virqs[32];            // SGI/PPI (per-CPU)
} vgic_vcpu_t;

// Global vGIC state
typedef struct vgic {
    struct gic_dist_map *dist;                // Emulated GICD registers
    virq_handle_t vspis[200];                 // SPI virq handles
    vgic_vcpu_t vgic_vcpu[MAX_CPUS];          // Per-VCPU state
} vgic_t;
```

**Maintenance handler** (`vgic_v2.c:330-345`):
```c
int vm_vgic_maintenance_handler(vm_vcpu_t *vcpu)
{
    int idx = seL4_GetMR(seL4_VGICMaintenance_IDX);  // Which LR triggered
    handle_vgic_maintenance(vcpu, idx);
    // → clear pending bit
    // → call virq_ack() callback
    // → clear lr_shadow[idx]
    // → dequeue next IRQ if any
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
    return VM_EXIT_HANDLED;
}
```

### 12.5 GICv2 Hardware vCPU Interface (Key Insight)

**Critical understanding**: GICv2 VMM does NOT emulate the CPU interface!

The GIC hardware provides a "virtual CPU interface" (GICV) at a separate physical address. The VMM maps this to the guest at the GICC address:

```c
// vgic_v2.c:318-321
vm_memory_reservation_t *vgic_vcpu_reservation = vm_reserve_memory_at(
    vm, GIC_CPU_PADDR, PAGE_SIZE_4K, handle_vgic_vcpu_fault, NULL);
vm_map_reservation(vm, vgic_vcpu_reservation, vgic_vcpu_iterator, vm);

// vgic_vcpu_iterator returns physical frame at GIC_VCPU_PADDR
// mapped to guest at GIC_CPU_PADDR
```

This means:
- Guest reads GICC_IAR → hardware returns pending IRQ from ICH_LRn
- Guest writes GICC_EOIR → hardware updates LR state, triggers maintenance
- VMM only handles GICD emulation and maintenance interrupts

### 12.6 GICv3 CPU Interface Options

For GICv3, the CPU interface uses system registers (ICC_*), not MMIO. Two approaches:

**Option A: Hardware vCPU mode (recommended)**
```
Configure ICC_SRE_EL2.Enable = 1 (allow guest ICC_* to reach hardware)
Guest: MRS x0, ICC_IAR1_EL1
   ↓
Hardware: ICH machinery returns pending IRQ (no trap to VMM)
   ↓
VMM: Only handles maintenance interrupts (same as GICv2)
```

**Pros**: Low overhead, hardware manages LR state
**Cons**: Less control over ICC_* emulation

**Option B: Full trap mode**
```
Configure ICC_SRE_EL2.Enable = 0 (trap all guest ICC_*)
Guest: MRS x0, ICC_IAR1_EL1
   ↓
EL2 trap: ESR.EC = 0x18 (MSR/MRS), ESR contains Op0/Op1/CRn/CRm/Op2
   ↓
Kernel: seL4_Fault_VCPUFault → VMM
   ↓
VMM: Decode ICC_* register, emulate read/write
```

**Pros**: Full control, can virtualize any ICC_* behavior
**Cons**: Higher overhead (trap on every ICC_* access)

**Recommendation**: Start with Option A (hardware vCPU mode) as it closely matches the existing GICv2 model and requires minimal VMM changes.

## 13. Additional Implementation Details

### 13.1 ICC_SRE_EL2 Register Layout

**Bits**:
- Bit 0: **SRE** - System Register Enable (enables ICC_* system registers for this EL)
- Bit 1: **DFB** - Disable FIQ Bypass
- Bit 2: **DIB** - Disable IRQ Bypass
- Bit 3: **Enable** - Enables lower EL access to ICC_SRE_EL1

**For hardware vCPU mode (recommended)**: No ICC_SRE_EL2 changes needed! The redirection happens via HCR_EL2.IMO/FMO.

**For full trap mode**: Clear ICC_SRE_EL2.Enable (bit 3) to trap guest ICC_SRE_EL1 accesses.

Sources: [ARM Developer Documentation](https://developer.arm.com/documentation/ddi0595/2021-06/AArch64-Registers/ICC-SRE-EL2--Interrupt-Controller-System-Register-Enable-register--EL2-), [Linux KVM vgic-v3.c](https://github.com/torvalds/linux/blob/master/arch/arm64/kvm/vgic/vgic-v3.c)

### 13.2 Hardware vCPU Mode (ICC→ICV Redirection)

**Key insight**: GICv3 provides automatic redirection of guest ICC_* accesses!

When `HCR_EL2.IMO=1` (already set by seL4 for VCPU virtualization):
- Guest executes `MRS x0, ICC_IAR1_EL1`
- Hardware automatically redirects to `ICV_IAR1_EL1`
- ICV registers interact with ICH_LRn_EL2 list registers
- **No trap to hypervisor** - hardware manages state

**seL4 kernel already sets HCR_EL2.IMO and HCR_EL2.FMO** (`vcpu.h:18`):
```c
#define HCR_COMMON ( HCR_VM | HCR_RW | HCR_AMO | HCR_IMO | HCR_FMO | HCR_TSC)
```

This means **hardware vCPU mode works out of the box** - no kernel changes needed for basic GICv3 support!

Sources: [ARM GICv3 Overview](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/GICv3_v4_overview.pdf), [OSDev Wiki](https://wiki.osdev.org/Generic_Interrupt_Controller_versions_3_and_4)

### 13.3 GICR_TYPER Register Layout

For redistributor emulation, GICR_TYPER must be constructed:

| Bits | Field | Value |
|------|-------|-------|
| [63:32] | Affinity_Value | MPIDR[23:0] of the vCPU |
| [23:8] | Processor_Number | vcpu_id (0-65535) |
| [4] | DPGS | 0 (no GICR_CTLR.DPG* support) |
| [0] | PLPIS | 1 if ITS/LPI supported, else 0 |
| [4] | Last | 1 if last redistributor in chain |

**Affinity must match MPIDR**: The guest's MPIDR_EL1 affinity levels must match GICR_TYPER.Affinity_Value.

**Example for single-vCPU VM**:
```c
uint64_t gicr_typer = 0;
gicr_typer |= ((uint64_t)(mpidr & 0xFFFFFF) << 32);  // Affinity
gicr_typer |= ((vcpu_id & 0xFFFF) << 8);              // Processor_Number
gicr_typer |= GICR_TYPER_LAST;                        // Last (single CPU)
// PLPIS = 0 (no LPI support initially)
```

Sources: [Linux KVM vgic-mmio-v3.c](https://lxr.missinglinkelectronics.com/linux+v5.16/arch/arm64/kvm/vgic/vgic-mmio-v3.c), [Kernel VGICv3 docs](https://www.kernel.org/doc/html/v5.6/virt/kvm/devices/arm-vgic-v3.html)

### 13.4 LPI/ITS for MSI Support

**LPIs (Locality-specific Peripheral Interrupts) are OPTIONAL in GICv3.**

| Feature | Required? | Notes |
|---------|-----------|-------|
| Basic GICv3 vGIC | No | SPIs/SGIs/PPIs work without LPIs |
| MSI-X passthrough | Yes | Requires ITS emulation |
| GICv4 direct inject | Hardware | Eliminates hypervisor involvement |

**For initial Orin AGX support**: Skip LPIs. Add ITS later if PCIe passthrough needed.

**GICD_TYPER.LPIS** indicates if LPIs are supported - set to 0 for no LPI support.

Sources: [Linaro KVM MSI Passthrough](https://old.linaro.org/blog/kvm-pciemsi-passthrough-armarm64/), [ARM GICv3 LPI Challenges](https://www.systemonchips.com/arm-gicv3-lpi-passthrough-challenges-and-priority-management/)

### 13.5 ICH_VTR_EL2 and Number of List Registers

**seL4 kernel already handles this dynamically!**

```c
// vcpu.c:21-25
gic_vcpu_num_list_regs = VGIC_VTR_NLISTREGS(get_gic_vcpu_ctrl_vtr());
if (gic_vcpu_num_list_regs > GIC_VCPU_MAX_NUM_LR) {
    gic_vcpu_num_list_regs = GIC_VCPU_MAX_NUM_LR;  // 16 for GICv3
}
```

**ICH_VTR_EL2.ListRegs**: bits [4:0], value 0-15 means 1-16 actual LRs.

The VMM should query this dynamically or use a safe default (4 is common minimum).

**VMM hardcoded value** (`virq.h:53`):
```c
#define NUM_LIST_REGS 4  // TODO: query from kernel
```

**Recommendation**: Add seL4 API to query `gic_vcpu_num_list_regs` or assume minimum of 4.

Sources: [Xen GICv3 fix](https://www.mail-archive.com/xen-devel@lists.xenproject.org/msg176781.html), [LKML vGIC helper](https://lkml.org/lkml/2025/12/17/656)

### 13.6 Revised Kernel Changes Summary

**Great news: Even fewer kernel changes needed than initially thought!**

| Change | Required? | Notes |
|--------|-----------|-------|
| ICC_SRE_EL2 configuration | **NO** | HCR_EL2.IMO already redirects ICC→ICV |
| ICH_LRn_EL2 access | Already done | `set_gic_vcpu_ctrl_lr()` in gic_v3.h |
| ICH_VTR_EL2 query | Already done | `gic_vcpu_num_list_regs` at boot |
| HCR_EL2.IMO/FMO | Already done | Set in HCR_COMMON |
| API for LR count | Optional | VMM currently hardcodes 4 |

**The seL4 kernel is already ready for GICv3 vGIC!** Only VMM changes are needed.

## Conclusion

GICv3 vGIC support is **even more tractable than initially expected**:

**Excellent news - NO kernel changes required for basic support:**
- seL4 kernel already sets HCR_EL2.IMO/FMO → hardware redirects guest ICC_* to ICV_*
- ICH_LRn_EL2 access functions already implemented in gic_v3.h
- `gic_vcpu_num_list_regs` already queried from ICH_VTR_EL2 at boot
- virq_t format already supports GICv3 64-bit layout
- VCPU fault forwarding works for any ICC_* that might trap
- Maintenance interrupts already handled via `seL4_Fault_VGICMaintenance`

**VMM-only work remaining:**
1. **GICR (Redistributor) emulation** (~800-1000 LOC)
   - Per-vCPU state for SGI/PPI enable, pending, priority
   - GICR_TYPER with correct Affinity_Value and Last bit
   - Two 64KB frames per vCPU (RD_base + SGI_base)

2. **GICD updates for GICv3** (~200 LOC)
   - GICD_IROUTER instead of GICD_ITARGETSR
   - GICD_CTLR.ARE_NS = 1 (affinity routing enabled)
   - GICD_TYPER updated for GICv3 features

3. **Platform configuration**
   - Define GICR base address and size for Orin AGX
   - Build system to select GICv2 vs GICv3

**Not required for initial support:**
- LPI/ITS (optional, needed only for MSI-X passthrough)
- Full ICC_* trap mode (hardware vCPU mode is simpler and faster)
- Kernel patches (everything needed is already there!)

The Linux KVM and Xen implementations can serve as references. **Total estimated VMM work: ~1200 LOC.**

## 14. Additional Technical Findings (Round 2)

### 14.1 Guest ICC_SRE_EL1 Handling

**Key insight: ICC_SRE_EL1 has NO ICV counterpart!**

Unlike other ICC_* registers which redirect to ICV_* when HCR_EL2.IMO=1:
- Guest reads ICC_SRE_EL1 → sees **actual hardware value** (not virtualized)
- Hypervisor must save/restore ICC_SRE_EL1 per vCPU

**From Linux KVM patches** ([Patchwork](https://patchwork.kernel.org/patch/4757661/)):
- ICC_SRE_EL1 is a per-VM variable
- Guest needs `ICC_SRE_EL1.SRE=1` to use system registers
- If EL2 sets `ICC_SRE_EL2.SRE=0`, the guest's ICC_SRE_EL1.SRE becomes RAZ/WI

**For our implementation**: Since HCR_EL2.IMO=1 redirects ICC→ICV automatically, and the kernel runs with SRE=1, guests will see SRE=1. No special handling needed.

Sources: [ARM ICC_SRE_EL1 Docs](https://dflund.se/~getz/ARM/SysReg/AArch64-icc_sre_el1.html), [Linux KVM Patch](https://patchwork.kernel.org/patch/4757661/)

### 14.2 Guest MPIDR_EL1 via VMPIDR_EL2

**Already implemented in libsel4vm!** (`vm.c:164-190`)

```c
// vcpu_start() in projects/sel4_projects_libs/libsel4vm/src/arch/arm/vm.c
if (vcpu->vcpu_id == BOOT_VCPU) {
    // VMPIDR_EL2: BIT(24)=MT, BIT(31)=MP extensions
    vmpidr_val = BIT(24) | BIT(31);  // = 0x81000000
} else {
    vmpidr_val = vcpu->target_cpu;
}
vm_set_arm_vcpu_reg(vcpu, seL4_VCPUReg_VMPIDR_EL2, vmpidr_val);
```

**Issue**: Current implementation doesn't set proper affinity levels!

For GICv3, GICR_TYPER.Affinity_Value must match guest MPIDR. Current code sets 0x81000000 which means:
- Aff0 = 0, Aff1 = 0, Aff2 = 0, Aff3 = 0

**Recommended fix for multi-vCPU VMs**:
```c
// Proper VMPIDR for vcpu_id N:
// Aff0 = vcpu_id & 0xFF, Aff1/2/3 = 0, MT=1, U=0
vmpidr_val = BIT(31) | BIT(24) | (vcpu_id & 0xFF);
```

**seL4 kernel CHANGES.md warning**: "The default value of [VMPIDR_EL2] is 0 which isn't a legal value for MPIDR_EL1 on AArch64"

### 14.3 GICR_WAKER Handling

**seL4 kernel behavior** (`gic_v3.c:225-234`):
```c
// Kernel expects GICR_WAKER = 0 (redistributor awake)
val = gic_rdist_map[core_id]->waker;
if (val & GICR_WAKER_ChildrenAsleep) {
    printf("GICv3: GICR_WAKER returned non-zero %x\n", val);
    halt();
}
```

**Register bits** (`gic_v3.h:63-64`):
- `GICR_WAKER_ProcessorSleep` = BIT(1) - set to 0 (awake)
- `GICR_WAKER_ChildrenAsleep` = BIT(2) - set to 0 (awake)

**VMM emulation**: Always return 0 for GICR_WAKER reads. Writes can be ignored (redistributor always awake).

### 14.4 ICH_VMCR_EL2 Bit Layout

**Complete bit layout** (from [ARM Developer Docs](https://developer.arm.com/documentation/ddi0601/latest/AArch64-Registers/ICH-VMCR-EL2--Interrupt-Controller-Virtual-Machine-Control-Register)):

| Bits | Field | Purpose |
|------|-------|---------|
| 0 | VENG0 | Virtual Enable Group 0 |
| 1 | VENG1 | Virtual Enable Group 1 |
| 2 | VACKCTL | Virtual AckCtl |
| 3 | VFIQEN | Virtual FIQ Enable |
| 4 | VCBPR | Virtual Common Binary Point Register |
| 9 | VEOIM | Virtual EOI Mode (alias of ICV_CTLR_EL1.EOImode) |
| 18-20 | VBPR1 | Virtual Binary Point Register Group 1 (3 bits) |
| 21-23 | VBPR0 | Virtual Binary Point Register Group 0 (3 bits) |
| 24-31 | VPMR | Virtual Priority Mask (8 bits) |

**seL4 kernel already handles ICH_VMCR_EL2** (`vcpu.c`):
```c
vcpu->vgic.vmcr = get_gic_vcpu_ctrl_vmcr();  // On VCPU save
set_gic_vcpu_ctrl_vmcr(vcpu->vgic.vmcr);     // On VCPU restore
```

**Initial value recommendation**: 0 is acceptable (all groups disabled, low priority mask). Guest Linux will configure properly during GIC init.

Sources: [ARM ICH_VMCR_EL2](https://df.lth.se/~getz/ARM/SysReg/AArch64-ich_vmcr_el2.html), [Jailhouse GICv3](https://github.com/siemens/jailhouse/blob/master/hypervisor/arch/arm-common/gic-v3.c)

### 14.5 GICR Frame Layout Details

**From seL4 kernel** (`gic_v3.c:14-18`):
```c
#define RDIST_BANK_SZ       0x00010000   // 64KB per frame
#define GICR_PER_CORE_SIZE  0x20000      // 128KB total (2 frames)
#define GICR_SIZE           0x100000     // 1MB for up to 8 CPUs
```

**Frame addresses** (`gic_v3.c:222-223`):
```c
gic_rdist_map[core_id] = (void *)gicr;              // RD_base
gic_rdist_sgi_ppi_map[core_id] = (void *)(gicr + RDIST_BANK_SZ);  // SGI_base
```

**Memory layout for vCPU N**:
```
GICR_BASE + (N * 0x20000) + 0x00000: RD_base (64KB)
    0x0000: GICR_CTLR
    0x0008: GICR_TYPER (64-bit)
    0x0014: GICR_WAKER
    0x0070: GICR_PROPBASER (LPI config)
    0x0078: GICR_PENDBASER (LPI pending)

GICR_BASE + (N * 0x20000) + 0x10000: SGI_base (64KB)
    0x0080: GICR_IGROUPR0
    0x0100: GICR_ISENABLER0
    0x0180: GICR_ICENABLER0
    0x0200: GICR_ISPENDR0
    0x0400: GICR_IPRIORITYR[0-7]
    0x0C00: GICR_ICFGR0/1
```

### 14.6 Tegra234 (Orin AGX) GIC Quirks

**Good news: No specific GICv3 quirks for Tegra234!**

Research findings:
- **T241-FABRIC-4 erratum** exists but is for T241 server chips only, not Tegra234
- The erratum affects multi-socket configurations (>2 chips) with GICv4
- Workaround disables GICv4 features (has_rvpeid, has_vlpis, has_direct_lpi)

**Tegra234 uses standard ARM GICv3** without known quirks. The GIC at 0xF400000 follows normal ARM GICv3 architecture.

Sources: [NVIDIA T241 Errata PDF](https://developer.nvidia.com/docs/t241-fabric-4/nvidia-t241-fabric-4-errata.pdf), [Linux Kernel Patch](https://lore.kernel.org/lkml/20230306013148.3483335-1-sdonthineni@nvidia.com/T/)

### 14.7 Build System for vGIC Selection

**Current state** (`libsel4vm/CMakeLists.txt:55-57`):
```cmake
if(KernelArchARM)
    list(APPEND sources src/arch/arm/vgic/vgic_v2.c)
endif()
```

**Hardcoded to GICv2 only!** Need conditional selection:

```cmake
if(KernelArchARM)
    if(KernelArmGICv3)
        list(APPEND sources src/arch/arm/vgic/vgic_v3.c)
    else()
        list(APPEND sources src/arch/arm/vgic/vgic_v2.c)
    endif()
endif()
```

**Platform detection**: `KernelArmGICv3` is set in kernel CMake config based on platform DTS.

### 14.8 Summary of Implementation Requirements

| Component | Status | Notes |
|-----------|--------|-------|
| ICC_SRE_EL1 | ✓ Ready | Hardware handles via HCR_EL2.IMO |
| VMPIDR_EL2 | ⚠️ Needs update | Add proper Aff0 for multi-vCPU |
| GICR_WAKER | To implement | Always return 0 (awake) |
| ICH_VMCR_EL2 | ✓ Ready | Kernel saves/restores |
| GICR frames | To implement | 128KB per vCPU |
| Tegra234 quirks | ✓ None | Standard GICv3 |
| Build system | To update | Add KernelArmGICv3 check |

**Priority order for implementation**:
1. Build system: Add GICv3/v2 selection
2. GICR RD_base: GICR_TYPER (with correct Affinity_Value), GICR_WAKER
3. GICR SGI_base: Enable/pending/priority for SGI/PPI
4. GICD updates: IROUTER, ARE_NS bit
5. VMPIDR fix: Set proper Aff0 = vcpu_id

## 15. LPI/ITS for MSI Support

### 15.1 Overview: LPI and ITS Architecture

**LPIs (Locality-specific Peripheral Interrupts)** are message-based interrupts introduced in GICv3:
- INTID range: 8192+ (configurable via GICD_TYPER.IDbits)
- Always edge-triggered, always Group 1 Non-secure
- Two states only: inactive or pending (no active state)
- Configuration stored in memory tables, not registers

**ITS (Interrupt Translation Service)** translates MSI writes to LPIs:
```
Device writes MSI → GITS_TRANSLATER (doorbell register)
                 ↓
ITS uses (DeviceID, EventID) to look up:
  Device Table → ITT → ITE → (LPI INTID, Collection ID)
                              ↓
                Collection Table → Redistributor target
                              ↓
                LPI delivered to target CPU
```

Sources: [ARM GICv3/v4 LPI Overview](https://documentation-service.arm.com/static/65ba2901c2052a35156cc629), [OSDev GICv3 Wiki](https://wiki.osdev.org/Generic_Interrupt_Controller_versions_3_and_4)

### 15.2 CRITICAL: Tegra234 Has NO ITS!

**Live system verification on Orin AGX:**
```
$ dmesg | grep ITS
(no output)

$ ls /proc/device-tree/bus@0/interrupt-controller@f400000/
compatible  interrupts  reg  ...  (no gic-its subnode!)

$ cat /proc/device-tree/bus@0/interrupt-controller@f400000/compatible
arm,gic-v3
```

**Tegra234 uses PCIe controller-integrated MSI instead:**
- DesignWare/Synopsys PCIe IP has built-in MSI controller
- MSI writes to PCIe controller → converted to GIC SPI interrupts
- No LPIs involved; MSI appears as normal SPI to software

```
$ dmesg | grep tegra.*pcie
tegra194-pcie 14100000.pcie: host bridge /bus@0/pcie@14100000 ranges:

$ cat /proc/interrupts | grep MSI
254:  PCI-MSI 134742016 Edge  rtl88x2ce
```

The kernel config has `CONFIG_ARM_GIC_V3_ITS=y` but no ITS hardware exists on Tegra234.

Sources: [pcie-tegra194.c](https://github.com/torvalds/linux/blob/master/drivers/pci/controller/dwc/pcie-tegra194.c), [NVIDIA PCIe DT Bindings](https://www.kernel.org/doc/Documentation/devicetree/bindings/pci/nvidia,tegra194-pcie.txt)

### 15.3 MSI Support Options for Tegra234 VMs

#### Option A: PCIe Controller Passthrough (Native Tegra)

Pass the PCIe controller (including MSI domain) directly to guest:
- Guest owns entire PCIe root complex
- MSI handled by PCIe controller hardware
- Requires IOMMU (SMMUv2 on Tegra234) for DMA isolation

**Pros**: No ITS emulation needed, hardware MSI performance
**Cons**: Entire PCIe controller dedicated to one VM

#### Option B: Virtual ITS Emulation (Generic GICv3)

Emulate ITS in VMM software for guest VMs:
```
Guest PCIe device writes MSI → trap to VMM
VMM: Emulated ITS translates (DeviceID, EventID) → virtual LPI
VMM: Inject vLPI via ICH_LRn_EL2
```

**Pros**: Multiple VMs can share PCIe, full control
**Cons**: Software overhead, complex implementation (~2000 LOC)

#### Option C: GICv2m Emulation (Current Approach)

The existing virtioso-camkes-vm uses GICv2m for MSI (see `src/msi.c`):
- Emulates GICv2m MSI frame at fixed address
- MSI writes converted to SPI injection
- Works with GICv3 in GICv2 compatibility mode

**Pros**: Already implemented and working
**Cons**: Limited to 128 MSI vectors, GICv2 style

**Recommendation**: Start with Option C (already working), add Option B later for full GICv3 MSI support.

### 15.4 ITS Register Layout (for Option B)

If implementing virtual ITS, these registers need emulation:

| Register | Offset | Purpose |
|----------|--------|---------|
| GITS_CTLR | 0x0000 | ITS control (enable/disable) |
| GITS_IIDR | 0x0004 | Implementer ID |
| GITS_TYPER | 0x0008 | ITS type (64-bit, device/collection bits) |
| GITS_CBASER | 0x0080 | Command queue base address |
| GITS_CWRITER | 0x0088 | Command queue write pointer |
| GITS_CREADR | 0x0090 | Command queue read pointer |
| GITS_BASER[0-7] | 0x0100-0x0138 | Device/Collection/vPE table bases |
| GITS_TRANSLATER | 0x10040 | MSI doorbell (write triggers translation) |

**Memory map**: ITS requires 128KB (64KB control + 64KB translation).

### 15.5 ITS Commands

Commands are 32-byte structures in the command queue:

| Command | Purpose |
|---------|---------|
| MAPD | Map device: DeviceID → ITT base address |
| MAPC | Map collection: CollectionID → Redistributor |
| MAPTI | Map trigger: (DeviceID, EventID) → (LPI INTID, CollectionID) |
| MAPI | Map interrupt: EventID == INTID (simplified MAPTI) |
| INV | Invalidate cached LPI configuration |
| INVALL | Invalidate all cached configurations |
| SYNC | Synchronize command completion |
| CLEAR | Clear pending state of LPI |
| DISCARD | Discard interrupt mapping |

**Command queue flow**:
1. Guest writes commands to queue (via GITS_CBASER)
2. Guest advances GITS_CWRITER
3. ITS processes commands, advances GITS_CREADR
4. GITS_CREADR == GITS_CWRITER means queue empty

Sources: [Linux KVM Virtual ITS](https://docs.kernel.org/virt/kvm/devices/arm-vgic-its.html), [QEMU arm_gicv3_its.c](https://github.com/qemu/qemu/blob/master/hw/intc/arm_gicv3_its.c)

### 15.6 LPI Configuration Tables

**LPI Configuration Table (via GICR_PROPBASER)**:
- One byte per LPI INTID
- Global: shared by all Redistributors
- Alignment: 4KB

```
Byte format for each LPI:
  [7:2] Priority (6 bits of 8-bit priority)
  [1]   Reserved
  [0]   Enable (1 = enabled)
```

**LPI Pending Table (via GICR_PENDBASER)**:
- One bit per LPI INTID
- Per-Redistributor: each CPU has its own
- Alignment: 64KB

**Configuration changes**:
1. Update byte in PROPBASER table
2. Issue INV or INVALL command to ITS
3. ITS invalidates cached configuration

### 15.7 Virtual ITS Implementation Strategy

If implementing virtual ITS (Option B):

**Phase 1: Basic ITS (no LPI delivery)**
1. Emulate GITS registers (CTLR, TYPER, CBASER, etc.)
2. Parse command queue (MAPD, MAPC, MAPTI)
3. Build internal DeviceID → LPI mapping tables
4. Trap GITS_TRANSLATER writes

**Phase 2: LPI Delivery**
1. On GITS_TRANSLATER write:
   - Look up (DeviceID, EventID) in tables
   - Find target LPI and Collection
   - Find target Redistributor from Collection
2. Set pending bit in virtual pending table
3. Inject LPI via ICH_LRn_EL2 (like any other virtual IRQ)

**Phase 3: LPI Configuration**
1. Emulate GICR_PROPBASER, GICR_PENDBASER
2. Handle INV/INVALL commands
3. Respect priority from configuration table

**Estimated effort**: ~2000-2500 LOC for full ITS emulation

### 15.8 GICv4 Direct LPI Injection (Future)

GICv4 adds hardware support for directly injecting virtual LPIs:
- vPE (virtual PE) tables in Redistributor
- Hardware looks up vLPI → physical LPI mapping
- No hypervisor trap on MSI delivery

**Not available on Tegra234** (GICv3 only), but relevant for future platforms.

### 15.9 Summary: MSI Support on Tegra234

| Approach | Complexity | Performance | Use Case |
|----------|------------|-------------|----------|
| GICv2m emulation | Low (done) | Good | Current implementation |
| PCIe passthrough | Medium | Best | Single VM per PCIe |
| Virtual ITS | High | Good | Multi-VM MSI sharing |

**For Orin AGX VMs**:
1. Use existing GICv2m emulation for now (works with GICv3)
2. Add virtual ITS later if more MSI vectors needed
3. PCIe passthrough for performance-critical devices

## 16. vhost Acceleration Requirements

### 16.1 Overview: vhost and MSI

vhost acceleration moves virtio data plane processing from QEMU userspace into the Device VM kernel, dramatically reducing I/O latency. The interrupt path for vhost uses irqfd (eventfd) to signal completion:

```
vhost-net (Device VM kernel)
    ↓ eventfd write
kmod-sel4-virt (sel4_irqfd_wakeywakey)
    ↓ sel4_vm_set_irqline(vm, virq, PULSE)
RPC: QEMU_OP_SET_IRQ
    ↓
VMM: handle_msi() → v2m_inject_irq()
    ↓
vm_inject_irq() → vGIC → Guest IRQ handler
```

**Key insight**: MSI injection for vhost is **RPC-based**, not a memory-mapped doorbell write. The kmod-sel4-virt sends the SPI number via RPC, and the VMM injects it through the vGIC.

### 16.2 GICv2m Compatibility with GICv3

**Good news: The existing GICv2m emulation works with GICv3!**

Linux supports `arm,gic-v2m-frame` with a GICv3 parent node. The device tree looks like:

```dts
/ {
    gic: interrupt-controller@f400000 {
        compatible = "arm,gic-v3";
        /* ... */
    };

    v2m: v2m@8020000 {
        compatible = "arm,gic-v2m-frame";
        msi-controller;
        msi-parent = <&gic>;
        arm,msi-base-spi = <144>;
        arm,msi-num-spis = <32>;
    };
};
```

The MSI mechanism remains the same - guest writes to GICv2m frame, which triggers an SPI. For vhost/irqfd, the RPC path bypasses the frame entirely and directly injects SPIs.

### 16.3 What Changes Between GICv2 and GICv3 for vhost

| Aspect | GICv2 vGIC | GICv3 vGIC | vhost Impact |
|--------|------------|------------|--------------|
| MSI mechanism | GICv2m frame | GICv2m frame (same!) | None |
| SPI injection | ICH_LR*_EL2 | ICH_LR*_EL2 (same!) | None |
| SPI routing | GICD_ITARGETSR (8-bit CPU mask) | GICD_IROUTER (64-bit affinity) | **Multi-vCPU only** |
| Per-queue vectors | Works | Works | None |
| Edge triggering | Works | Works | None |

**For single-vCPU VMs: No changes needed!**

### 16.4 Multi-vCPU SPI Routing (Future)

For multi-queue virtio-net with per-CPU MSI affinity (the main vhost performance case), GICv3 uses `GICD_IROUTER[N]` instead of `GICD_ITARGETSR[N]`:

**Current GICv2 vGIC** (`vgic_v2.c:169-171`):
```c
// All SPIs hardcoded to CPU 0
for (int i = 0; i < ARRAY_SIZE(gic_dist->targets); i++) {
    gic_dist->targets[i] = 0x1010101;  // CPU 0 for all
}
```

**GICv3 vGIC needs**:
```c
// GICD_IROUTER[N] - 64-bit per SPI
// Bits [39:32] = Aff3, [23:16] = Aff2, [15:8] = Aff1, [7:0] = Aff0
// Must match VMPIDR_EL2 of target vCPU
uint64_t irouter[988];  // SPIs 32-1019
```

Guest Linux would configure:
```
GICD_IROUTER[148] = 0x00000000  // MSI vector 0 → vCPU 0 (Aff0=0)
GICD_IROUTER[149] = 0x00000001  // MSI vector 1 → vCPU 1 (Aff0=1)
GICD_IROUTER[150] = 0x00000002  // MSI vector 2 → vCPU 2 (Aff0=2)
GICD_IROUTER[151] = 0x00000003  // MSI vector 3 → vCPU 3 (Aff0=3)
```

**Implementation requirements**:
1. Emulate GICD_IROUTER reads/writes (offset 0x6100-0x7FD8)
2. Store 64-bit affinity value per SPI
3. Modify `vm_inject_irq()` to route to correct vCPU based on IROUTER
4. Ensure VMPIDR_EL2 is properly set (see Section 14.2)

### 16.5 Current vhost Code Path

**kmod-sel4-virt** (`sel4_irqfd.c:30-37`):
```c
static void sel4_irqfd_inject(struct sel4_irqfd *irqfd)
{
    u64 cnt;
    eventfd_ctx_do_read(irqfd->eventfd, &cnt);
    /* Pulse irq */
    sel4_vm_set_irqline(irqfd->vm, irqfd->virq, SEL4_IRQ_OP_PULSE);
}
```

**VMM MSI handler** (`src/plat/rpi4/msi.c:20-41`):
```c
int msi_irq_set(uint32_t irq, uint32_t op)
{
    if (!v2m_irq_valid(&v2m, irq)) {
        return RPCMSG_RC_NONE;  // Not MSI, try other handlers
    }
    switch (op) {
    case RPC_IRQ_SET:
    case RPC_IRQ_PULSE:
        v2m_inject_irq(&v2m, irq);  // → irq_line_pulse()
        break;
    }
    return RPCMSG_RC_HANDLED;
}
```

**IRQ injection** (`src/irq_line.c:36-43`):
```c
int irq_line_pulse(irq_line_t *line)
{
    int err = vm_set_irq_level(line->vcpu, line->irq, true);
    if (err) return err;
    return vm_set_irq_level(line->vcpu, line->irq, false);
}
```

Note: Currently `irq_line_init()` binds the IRQ to `BOOT_VCPU` (vCPU 0). For multi-vCPU routing, this would need to dynamically select the target vCPU based on IROUTER.

### 16.6 Implementation Phases for vhost Support

**Phase 1: Basic GICv3 vGIC (single-vCPU)**
- Keep GICv2m emulation unchanged
- Implement basic GICR (Redistributor) for PPIs
- All SPIs still go to vCPU 0
- **vhost works with no changes**

**Phase 2: Multi-vCPU SPI Routing**
- Add GICD_IROUTER[N] emulation (64-bit per SPI)
- Modify `vm_inject_irq()` to route based on affinity
- Update `irq_line_t` to support dynamic vCPU targeting
- Fix VMPIDR_EL2 to set proper Aff0 = vcpu_id
- **Enables full multi-queue vhost performance**

**Phase 3: Virtual ITS (optional)**
- Only if >32 MSI vectors needed
- Implements LPI translation (DeviceID, EventID) → virtual LPI
- ~2000-2500 LOC additional (see Section 15)

### 16.7 Summary: vhost and GICv3

| Configuration | vhost Works? | Changes Needed |
|---------------|--------------|----------------|
| GICv3 + single vCPU | ✓ Yes | None - GICv2m works as-is |
| GICv3 + multi vCPU (all IRQs to vCPU 0) | ✓ Yes | None |
| GICv3 + multi vCPU (per-CPU affinity) | Needs IROUTER | Phase 2 |
| GICv3 + >32 MSI vectors | Needs virtual ITS | Phase 3 |

**Recommendation**: Start with Phase 1. The existing GICv2m + basic GICv3 vGIC gives full vhost functionality for single-vCPU VMs. Add multi-vCPU routing when needed.

## 17. Phase 1 Implementation Details

This section documents the previously unclear items for Phase 1 (basic GICv3 vGIC for single-vCPU VMs).

### 17.1 Guest Device Tree Generation

**Question**: How does the guest get its GICv3 device tree node?

**Answer**: The CAmkES VM framework **keeps the GIC node from the platform device tree**.

The `plat_keep_devices[]` array in `plat_include/<platform>/plat/vmlinux.h` specifies which nodes to preserve:

```c
// projects/vm/components/VM_Arm/plat_include/orinagx/plat/vmlinux.h
#define GIC_NODE_PATH "/interrupt-controller@f400000"

static const char *plat_keep_devices[] = {
    "/timer",
    "/psci",
    GIC_NODE_PATH,  // GIC node is kept from platform DTB
};
```

**For Orin AGX**: The GICv3 node at `/interrupt-controller@f400000` is preserved from the Tegra234 device tree. The guest sees:

```dts
interrupt-controller@f400000 {
    compatible = "arm,gic-v3";
    reg = <0x0 0x0f400000 0x0 0x10000>,   /* GICD */
          <0x0 0x0f440000 0x0 0x200000>;  /* GICR */
    interrupt-controller;
    #interrupt-cells = <3>;
    /* ... */
};
```

**No generation code needed** - the device tree comes from the platform. However, the VMM must:
1. Intercept MMIO accesses to GICD (0x0F400000) and GICR (0x0F440000)
2. Emulate the registers instead of passing through to hardware

**Key properties for GICv3**:
- `compatible = "arm,gic-v3"` - identifies GICv3
- `reg` - must include both GICD and GICR regions
- `#redistributor-regions = <1>` - optional, defaults to 1

Sources: [Linux GICv3 DT bindings](https://www.kernel.org/doc/Documentation/devicetree/bindings/interrupt-controller/arm,gic-v3.yaml)

### 17.2 SGI Generation Mechanism

**Question**: How do SGIs work in GICv3 vGIC? Does hardware handle them?

**Answer**: **ICC_SGI*R_EL1 does NOT have an ICV counterpart - it TRAPS to EL2!**

Unlike other ICC_* registers (which redirect to ICV_* when HCR_EL2.IMO=1), the SGI generation registers **always trap**:

| Register | When HCR_EL2.IMO=1 | Notes |
|----------|-------------------|-------|
| ICC_IAR1_EL1 | Redirects to ICV_IAR1_EL1 | Hardware handles |
| ICC_EOIR1_EL1 | Redirects to ICV_EOIR1_EL1 | Hardware handles |
| ICC_PMR_EL1 | Redirects to ICV_PMR_EL1 | Hardware handles |
| **ICC_SGI1R_EL1** | **Traps to EL2** | VMM must handle! |
| **ICC_SGI0R_EL1** | **Traps to EL2** | VMM must handle! |
| **ICC_ASGI1R_EL1** | **Traps to EL2** | VMM must handle! |

From [Linux KVM SGI trapping patch](https://patchwork.kernel.org/project/linux-arm-kernel/patch/1403171152-24067-13-git-send-email-andre.przywara@arm.com/):
> "While the injection of a (virtual) inter-processor interrupt (SGI) on a GICv2 works by writing to a MMIO register, GICv3 uses system registers to trigger them. The appropriate registers are trapped on both ARM and ARM64 machines and call the SGI handler function in the vGICv3 emulation code."

**For Phase 1 (single-vCPU)**: SGIs are not relevant!
- Self-SGI (sending to own CPU) is the only possible use case
- Can be handled by simply injecting the SGI to the same vCPU
- No cross-vCPU routing needed

**For Phase 2 (multi-vCPU)**:
1. Trap ICC_SGI1R_EL1 writes via ESR decoding
2. Parse affinity and target list from register value:
   ```
   ICC_SGI1R_EL1 format:
   [55:48] Aff3, [39:32] Aff2, [23:16] Aff1, [15:0] TargetList
   [40] IRM (Interrupt Routing Mode: 0=use TargetList, 1=all-but-self)
   [27:24] INTID (SGI number 0-15)
   ```
3. Find target vCPUs by matching affinity
4. Inject SGI to each target via `vm_inject_irq()`

**seL4 kernel already forwards SGI traps**: The ESR contains EC=0x18 (MSR/MRS trap) with the register encoding. VMM receives this via `seL4_Fault_VCPUFault`.

Sources: [ARM Community Forum](https://community.arm.com/support-forums/f/architectures-and-processors-forum/45594/aarch64-gicv3-icc_sgi1r_el1-aff1), [KVM SGI Patch](https://patchwork.kernel.org/project/linux-arm-kernel/patch/1414776414-13426-18-git-send-email-andre.przywara@arm.com/)

### 17.3 GICD_CTLR Initialization

**Question**: What should the GICD_CTLR reset value be?

**Answer**: Force `ARE_NS=1` and `DS=1`, enable Group 1 interrupts.

From [Linux KVM vgic-mmio-v3.c](https://lxr.missinglinkelectronics.com/linux+v5.16/arch/arm64/kvm/vgic/vgic-mmio-v3.c):
> "The GICv3 emulation is limited to a model enforcing a single security state, with SRE==1 (forcing system register access) and ARE==1 (allowing more than 8 VCPUs)."

**GICD_CTLR layout for GICv3**:

| Bit | Field | Value | Notes |
|-----|-------|-------|-------|
| 0 | EnableGrp0 | 0 | Group 0 disabled (optional) |
| 1 | EnableGrp1NS | 1 | Group 1 Non-secure enabled |
| 2 | EnableGrp1S | 0 | Group 1 Secure (not used) |
| 4 | ARE_S | 1 | Affinity routing (secure) - forced |
| 5 | ARE_NS | 1 | Affinity routing (non-secure) - forced |
| 6 | DS | 1 | Disable Security - forced |
| 31 | RWP | 0 | Register Write Pending (RO) |

**Implementation**:
```c
// GICv3 vGIC distributor reset
void vgic_v3_dist_reset(struct gic_dist_map *gic_dist) {
    memset(gic_dist, 0, sizeof(*gic_dist));

    // GICD_CTLR: Force ARE_NS=1, DS=1, enable Group 1 NS
    gic_dist->ctlr = GICD_CTLR_ARE_NS | GICD_CTLR_DS | GICD_CTLR_ENABLE_G1NS;

    // GICD_TYPER: Report GICv3 features
    // ITLinesNumber = (MAX_SPI / 32) - 1
    // CPUNumber = 0 (use GICR for CPU count)
    // IDbits = 9 (10-bit INTID, up to 1020)
    gic_dist->typer = ((MAX_SPI / 32) - 1) | (9 << 19);

    // Other initializations similar to GICv2...
}
```

**Guest writes to GICD_CTLR**: ARE_NS and DS bits should be RAO/WI (read-as-one, write-ignored). KVM enforces this by always ORing in these bits on read.

Sources: [LWN KVM GICv3 emulation](https://lwn.net/Articles/620979/), [KVM vgic-mmio-v3.c](https://zerodayengineering.com/content/kvm-docs/v6.8.0-38-arm64/vgic-mmio-v3_8c_source.html)

### 17.4 GICR Memory Reservation Strategy

**Question**: How should GICR memory regions be reserved?

**Answer**: Single reservation for entire GICR region with internal offset calculation.

**GICR layout** (from Section 14.5):
```
GICR_BASE + (vcpu_id * 0x20000) + 0x00000: RD_base (64KB)
GICR_BASE + (vcpu_id * 0x20000) + 0x10000: SGI_base (64KB)
```

**For Orin AGX**:
- GICR_BASE = 0x0F440000
- GICR_SIZE = 0x200000 (2MB, supports up to 16 CPUs)
- Per-vCPU size = 0x20000 (128KB)

**Implementation approach**:

```c
// Single reservation for entire GICR region
vm_memory_reservation_t *gicr_reservation;

int vgic_v3_init_gicr(vm_t *vm, uintptr_t gicr_base, size_t gicr_size) {
    // Reserve entire GICR region with single fault handler
    gicr_reservation = vm_reserve_memory_at(
        vm,
        gicr_base,      // 0x0F440000
        gicr_size,      // 0x200000
        handle_gicr_fault,
        (void *)vgic
    );
    return gicr_reservation ? 0 : -1;
}

// Fault handler calculates which vCPU and frame
memory_fault_result_t handle_gicr_fault(vm_t *vm, vm_vcpu_t *vcpu,
                                         uintptr_t paddr, size_t len,
                                         void *cookie) {
    vgic_t *vgic = cookie;
    uintptr_t offset = paddr - GICR_BASE;

    // Calculate vCPU index and frame
    int vcpu_idx = offset / 0x20000;
    uintptr_t frame_offset = offset % 0x20000;

    bool is_sgi_base = (frame_offset >= 0x10000);
    uintptr_t reg_offset = frame_offset & 0xFFFF;

    if (is_sgi_base) {
        return handle_gicr_sgi_fault(vgic, vcpu, vcpu_idx, reg_offset, len);
    } else {
        return handle_gicr_rd_fault(vgic, vcpu, vcpu_idx, reg_offset, len);
    }
}
```

**Why single reservation**:
- Simpler than per-vCPU reservations
- Linear address space matches hardware layout
- Fault handler can easily compute target vCPU

Sources: [Linux KVM GICR](https://lxr.missinglinkelectronics.com/linux+v5.16/arch/arm64/kvm/vgic/vgic-mmio-v3.c)

### 17.5 Testing Strategy

**Question**: How do we verify GICv3 vGIC works?

**Answer**: Use timer interrupt (PPI 27) for minimal test, Linux guest for comprehensive test.

**Test 1: Timer Interrupt (Minimal)**

The ARM virtual timer generates PPI 27. If vGIC works, the guest receives timer interrupts:

```c
// Minimal test in guest (or sel4test with VCPU)
void test_vgic_timer(void) {
    // Enable timer
    uint64_t cval = read_cntpct_el0() + 1000000;  // 1M cycles
    write_cntp_cval_el0(cval);
    write_cntp_ctl_el0(1);  // Enable

    // Wait for interrupt
    wfi();

    // If we get here, timer interrupt was delivered
    // Read IAR, write EOIR
}
```

For this to work:
1. GICR must emulate GICR_ISENABLER0 (enable PPI 27)
2. GICR must emulate GICR_IPRIORITYR (set priority)
3. ICH_LRn_EL2 must be loaded with pending interrupt
4. Maintenance interrupt must be handled on EOI

**Test 2: Linux Guest (Comprehensive)**

Linux exercises GIC during boot:
1. Reads GICD_TYPER, GICD_IIDR
2. Configures GICD_IGROUPR, GICD_ISENABLER
3. For each CPU: reads GICR_TYPER, writes GICR_WAKER
4. Enables timer interrupt (PPI 27)
5. If all works, reaches userspace

**Test sequence**:
```bash
# Build vm_minimal for Orin AGX
make orinagx_defconfig
make vm_minimal

# Boot and check console output
# Success: "Welcome to Linux" or shell prompt
# Failure: Hang at "Booting Linux..." or GIC error messages
```

**Debug checklist if Linux hangs**:
1. Add printf in GICD fault handler - is it being called?
2. Check GICR fault handler - is GICR_WAKER read returning 0?
3. Check GICR_TYPER - does Affinity_Value match MPIDR?
4. Check timer interrupt - is PPI 27 enabled and injected?

**sel4test vCPU tests**: Currently minimal (VCPU0001 only tests injection without TCB). Could be extended to test GICv3 vGIC.

### 17.6 File Structure and API

**Question**: What files and functions are needed for vgic_v3.c?

**Answer**: Same API as vgic_v2.c with additional GICR handling.

**New files**:
```
libsel4vm/src/arch/arm/vgic/
├── vgic_v3.c           # Main GICv3 emulation (~800 LOC)
├── vgicv3_defs.h       # GICv3 register definitions
├── gicv3.h             # Platform addresses
└── vgicr.h             # Redistributor state (optional)
```

**Public API** (unchanged from GICv2):
```c
// Install vGIC for a VM
int vm_install_vgic(vm_t *vm);

// Handle maintenance interrupt
int vm_vgic_maintenance_handler(vm_vcpu_t *vcpu);

// Inject IRQ to guest
int vm_inject_irq(vm_vcpu_t *vcpu, int irq);

// Set IRQ level (for level-triggered)
int vm_set_irq_level(vm_vcpu_t *vcpu, int irq, int level);

// Register IRQ with ack callback
int vm_register_irq(vm_vcpu_t *vcpu, int irq, irq_ack_fn_t ack, void *cookie);
```

**Internal structures**:
```c
// GICv3 distributor state
struct gic_v3_dist_map {
    uint32_t ctlr;              // GICD_CTLR
    uint32_t typer;             // GICD_TYPER
    uint32_t iidr;              // GICD_IIDR
    uint32_t igroupr[32];       // GICD_IGROUPR
    uint32_t isenabler[32];     // GICD_ISENABLER
    uint32_t icenabler[32];     // GICD_ICENABLER
    uint32_t ispendr[32];       // GICD_ISPENDR
    uint32_t icpendr[32];       // GICD_ICPENDR
    uint32_t ipriorityr[256];   // GICD_IPRIORITYR
    uint32_t icfgr[64];         // GICD_ICFGR
    uint64_t irouter[988];      // GICD_IROUTER (SPIs 32-1019)
};

// Per-vCPU redistributor state
struct gic_v3_redist_map {
    uint32_t ctlr;              // GICR_CTLR
    uint64_t typer;             // GICR_TYPER
    uint32_t waker;             // GICR_WAKER (always 0)
    // SGI_base registers
    uint32_t igroupr0;          // GICR_IGROUPR0
    uint32_t isenabler0;        // GICR_ISENABLER0
    uint32_t icenabler0;        // GICR_ICENABLER0
    uint32_t ispendr0;          // GICR_ISPENDR0
    uint32_t icpendr0;          // GICR_ICPENDR0
    uint32_t ipriorityr[8];     // GICR_IPRIORITYR (SGI/PPI)
    uint32_t icfgr[2];          // GICR_ICFGR0/1
};
```

**Build system** (CMakeLists.txt update):
```cmake
if(KernelArchARM)
    if(KernelArmGicV3)
        list(APPEND sources src/arch/arm/vgic/vgic_v3.c)
    else()
        list(APPEND sources src/arch/arm/vgic/vgic_v2.c)
    endif()
endif()
```

**Platform detection**: `KernelArmGicV3` is set in kernel CMake based on platform config. For Orin AGX, this is already true.

### 17.7 Phase 1 Implementation Checklist

| Task | Complexity | Status |
|------|------------|--------|
| Create vgicv3_defs.h with register offsets | Low | ✅ DONE |
| Create gicv3.h with platform addresses | Low | ✅ DONE |
| Implement GICD fault handler (ARE_NS=1, DS=1) | Medium | ✅ DONE |
| Implement GICR RD_base handler (TYPER, WAKER) | Medium | ✅ DONE |
| Implement GICR SGI_base handler (enable, priority) | Medium | ✅ DONE (partial - see Section 20) |
| Update CMakeLists.txt for GICv3 selection | Low | ✅ DONE |
| Test with timer interrupt | Medium | ✅ DONE |
| Test with Linux guest | High | ⚠️ IN PROGRESS (BPMP issue - see Section 20) |

**Estimated LOC**: ~800-1000 for basic GICv3 vGIC

**Not needed for Phase 1**:
- GICD_IROUTER (all SPIs go to vCPU 0)
- ICC_SGI*R trapping (single vCPU, no cross-vCPU SGIs)
- LPI/ITS support (GICv2m works for MSI)
- Multi-vCPU VMPIDR fix

### 17.8 Clarifications for Phase 1 Implementation

This section documents design decisions for previously unclear aspects of Phase 1.

#### 17.8.1 Platform Address Source

**Decision**: Define in platform-specific header `gicv3.h` with compile-time `#ifdef`.

```c
// libsel4vm/src/arch/arm/vgic/gicv3.h

#ifndef __VGIC_GICV3_H__
#define __VGIC_GICV3_H__

#if defined(CONFIG_PLAT_ORINAGX)
    #define GIC_DIST_PADDR      0x0F400000
    #define GIC_DIST_SIZE       0x10000      /* 64KB */
    #define GIC_REDIST_PADDR    0x0F440000
    #define GIC_REDIST_SIZE     0x200000     /* 2MB */
    #define GIC_REDIST_STRIDE   0x20000      /* 128KB per CPU */
#elif defined(CONFIG_PLAT_QEMU_ARM_VIRT)
    #define GIC_DIST_PADDR      0x08000000
    #define GIC_DIST_SIZE       0x10000
    #define GIC_REDIST_PADDR    0x080A0000
    #define GIC_REDIST_SIZE     0x100000
    #define GIC_REDIST_STRIDE   0x20000
#else
    #error "GICv3 addresses not defined for this platform"
#endif

#endif /* __VGIC_GICV3_H__ */
```

**Rationale**:
- Matches GICv2 pattern (`gicv2.h` has similar `#ifdef` blocks)
- Compile-time constants enable optimization
- Device tree parsing is complex and not needed for fixed-address platforms

#### 17.8.2 Unimplemented Register Behavior

**Decision**: RAZ/WI (Read-As-Zero, Write-Ignored) with optional debug logging.

```c
// In handle_vgic_dist_fault():
default:
    // RAZ/WI for unimplemented registers
    if (fault_is_read(vcpu)) {
        fault_set_data(vcpu, 0);  // RAZ
    }
    // Writes silently ignored (WI)

#ifdef CONFIG_DEBUG_BUILD
    ZF_LOGW("GICD: unhandled %s at offset 0x%lx",
            fault_is_read(vcpu) ? "read" : "write", offset);
#endif
    break;
```

**Specific register handling**:

| Register | Behavior | Rationale |
|----------|----------|-----------|
| `GICD_IGRPMODR` | RAZ/WI | Security extension, DS=1 makes it irrelevant |
| `GICR_PROPBASER` | RAZ/WI | LPI not supported in Phase 1 |
| `GICR_PENDBASER` | RAZ/WI | LPI not supported in Phase 1 |
| Reserved offsets | RAZ/WI | ARM recommends RAZ/WI for reserved |
| `GICD_IROUTER[n]` | Return 0 | Phase 1 routes all SPIs to vCPU 0 |

**Rationale**: RAZ/WI is the ARM-recommended behavior for unimplemented features and matches Linux KVM's approach.

#### 17.8.3 vm_install_vgic() Entry Point Architecture

**Decision**: Single entry point with compile-time selection via `#ifdef`.

```c
// libsel4vm/src/arch/arm/vgic/vgic.c (new dispatcher file)

#include <sel4vm/guest_vm.h>

#ifdef CONFIG_ARM_GIC_V3
extern int vm_install_vgic_v3(vm_t *vm);
extern int vm_vgic_v3_maintenance_handler(vm_vcpu_t *vcpu);
#else
extern int vm_install_vgic_v2(vm_t *vm);
extern int vm_vgic_v2_maintenance_handler(vm_vcpu_t *vcpu);
#endif

// Single public API - implementation selected at compile time
int vm_install_vgic(vm_t *vm) {
#ifdef CONFIG_ARM_GIC_V3
    return vm_install_vgic_v3(vm);
#else
    return vm_install_vgic_v2(vm);
#endif
}

int vm_vgic_maintenance_handler(vm_vcpu_t *vcpu) {
#ifdef CONFIG_ARM_GIC_V3
    return vm_vgic_v3_maintenance_handler(vcpu);
#else
    return vm_vgic_v2_maintenance_handler(vcpu);
#endif
}
```

**CMakeLists.txt update**:
```cmake
if(KernelArchARM)
    list(APPEND sources src/arch/arm/vgic/vgic.c)  # Common entry point
    if(KernelArmGicV3)
        list(APPEND sources src/arch/arm/vgic/vgic_v3.c)
        target_compile_definitions(sel4vm PRIVATE CONFIG_ARM_GIC_V3)
    else()
        list(APPEND sources src/arch/arm/vgic/vgic_v2.c)
    endif()
endif()
```

**Rationale**:
- Platform is known at build time; runtime detection adds complexity with no benefit
- Avoids linking both v2 and v3 code
- Matches seL4 kernel's compile-time GIC version selection

#### 17.8.4 List Register Count

**Decision**: Keep hardcoded value (4) for Phase 1, document as technical debt.

```c
// libsel4vm/src/arch/arm/vgic/virq.h

/*
 * Number of list registers (ICH_LRn_EL2) available for virtual IRQ injection.
 *
 * The actual count is in kernel's gic_vcpu_num_list_regs (read from ICH_VTR_EL2).
 * ARM GICv3 guarantees minimum of 4, maximum of 16.
 *
 * TODO: Add seL4 API to query actual LR count. For now, use conservative minimum.
 */
#define NUM_LIST_REGS 4
```

**Rationale**:
- 4 is the ARM-guaranteed minimum for GICv3
- Adding a new seL4 syscall is out of scope for Phase 1
- Real workloads rarely need >4 concurrent pending interrupts
- Overflow is handled by software queue in VMM (already implemented)

**Future enhancement** (Phase 2+): Add `seL4_ARM_VCPU_GetNumListRegs()` syscall.

#### 17.8.5 Fault Handler Return Values

**Decision**: Always return `FAULT_HANDLED` after advancing PC; use `FAULT_ERROR` only for fatal errors.

```c
// Fault handling pattern for GICv3 vGIC

static memory_fault_result_t handle_gicd_fault(vm_t *vm, vm_vcpu_t *vcpu,
                                                uintptr_t paddr, size_t len,
                                                void *cookie) {
    vgic_t *vgic = (vgic_t *)cookie;
    uintptr_t offset = paddr - GIC_DIST_PADDR;

    // Validate alignment
    if (offset & 0x3) {
        ZF_LOGE("GICD: unaligned access at 0x%lx", paddr);
        return FAULT_ERROR;  // Unaligned MMIO is a guest bug
    }

    // Handle the access (known or unknown register)
    if (fault_is_read(vcpu)) {
        uint32_t value = gicd_read(vgic, offset);
        fault_set_data(vcpu, value);
    } else {
        uint32_t value = fault_get_data(vcpu);
        gicd_write(vgic, offset, value);
    }

    // Advance PC past the faulting instruction
    return advance_fault(vcpu);  // Returns FAULT_HANDLED
}
```

**Return value semantics**:

| Return Value | When to Use |
|--------------|-------------|
| `FAULT_HANDLED` | Normal case - fault emulated, PC advanced |
| `FAULT_UNHANDLED` | **Never for vGIC** - we own the entire GICD/GICR region |
| `FAULT_ERROR` | Fatal error: unaligned access, internal bug, should-not-happen |

**Rationale**:
- `FAULT_UNHANDLED` is for regions where multiple handlers might apply
- vGIC exclusively owns GICD and GICR address ranges
- Any access within those ranges must be handled by vGIC
- Unknown offsets get RAZ/WI treatment (see 17.8.2)

### 17.9 Phase 1 Design Decisions Summary

| Item | Decision | Complexity |
|------|----------|------------|
| Platform addresses | Compile-time `#ifdef` in `gicv3.h` | Low |
| Unimplemented registers | RAZ/WI with debug logging | Low |
| Entry point architecture | Single `vm_install_vgic()` with compile-time selection | Low |
| List register count | Keep hardcoded 4, document as TODO | None |
| Fault return values | Always `FAULT_HANDLED`, `FAULT_ERROR` for bugs only | Low |

All design decisions maintain consistency with the existing GICv2 implementation.

## 18. Phase 1 Implementation Roadmap

This section breaks down Phase 1 into small, reviewable commits. Each commit is self-contained and builds on the previous one.

### Commit 1: Add GICv3 register definitions header

**Files**: `libsel4vm/src/arch/arm/vgic/vgicv3_defs.h`

Create header with GICv3 register offset definitions:

```c
// GICD offsets
#define GICD_CTLR           0x0000
#define GICD_TYPER          0x0004
#define GICD_IIDR           0x0008
#define GICD_IGROUPR(n)     (0x0080 + (n) * 4)
#define GICD_ISENABLER(n)   (0x0100 + (n) * 4)
#define GICD_ICENABLER(n)   (0x0180 + (n) * 4)
#define GICD_ISPENDR(n)     (0x0200 + (n) * 4)
#define GICD_ICPENDR(n)     (0x0280 + (n) * 4)
#define GICD_ISACTIVER(n)   (0x0300 + (n) * 4)
#define GICD_ICACTIVER(n)   (0x0380 + (n) * 4)
#define GICD_IPRIORITYR(n)  (0x0400 + (n) * 4)
#define GICD_ITARGETSR(n)   (0x0800 + (n) * 4)  // GICv2 compat, not used
#define GICD_ICFGR(n)       (0x0C00 + (n) * 4)
#define GICD_IGRPMODR(n)    (0x0D00 + (n) * 4)
#define GICD_IROUTER(n)     (0x6000 + (n) * 8)  // 64-bit per SPI

// GICD_CTLR bits
#define GICD_CTLR_ENABLE_G0     BIT(0)
#define GICD_CTLR_ENABLE_G1NS   BIT(1)
#define GICD_CTLR_ENABLE_G1S    BIT(2)
#define GICD_CTLR_ARE_S         BIT(4)
#define GICD_CTLR_ARE_NS        BIT(5)
#define GICD_CTLR_DS            BIT(6)
#define GICD_CTLR_RWP           BIT(31)

// GICR RD_base offsets (frame 0)
#define GICR_CTLR           0x0000
#define GICR_IIDR           0x0004
#define GICR_TYPER          0x0008  // 64-bit
#define GICR_WAKER          0x0014
#define GICR_PROPBASER      0x0070  // 64-bit, LPI
#define GICR_PENDBASER      0x0078  // 64-bit, LPI

// GICR SGI_base offsets (frame 1, +0x10000)
#define GICR_SGI_BASE       0x10000
#define GICR_IGROUPR0       (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0     (GICR_SGI_BASE + 0x0100)
#define GICR_ICENABLER0     (GICR_SGI_BASE + 0x0180)
#define GICR_ISPENDR0       (GICR_SGI_BASE + 0x0200)
#define GICR_ICPENDR0       (GICR_SGI_BASE + 0x0280)
#define GICR_ISACTIVER0     (GICR_SGI_BASE + 0x0300)
#define GICR_ICACTIVER0     (GICR_SGI_BASE + 0x0380)
#define GICR_IPRIORITYR(n)  (GICR_SGI_BASE + 0x0400 + (n) * 4)
#define GICR_ICFGR0         (GICR_SGI_BASE + 0x0C00)
#define GICR_ICFGR1         (GICR_SGI_BASE + 0x0C04)

// GICR_TYPER bits
#define GICR_TYPER_PLPIS        BIT(0)
#define GICR_TYPER_VLPIS        BIT(1)
#define GICR_TYPER_LAST         BIT(4)
```

**Test**: Compiles (header-only, no functional test)

---

### Commit 2: Add GICv3 platform addresses header

**Files**: `libsel4vm/src/arch/arm/vgic/gicv3.h`

Create header with platform-specific GIC addresses:

```c
#pragma once

#if defined(CONFIG_PLAT_ORINAGX)
    #define GIC_V3_DIST_PADDR       0x0F400000
    #define GIC_V3_DIST_SIZE        0x10000      /* 64KB */
    #define GIC_V3_REDIST_PADDR     0x0F440000
    #define GIC_V3_REDIST_SIZE      0x200000     /* 2MB */
    #define GIC_V3_REDIST_STRIDE    0x20000      /* 128KB per CPU */
#elif defined(CONFIG_PLAT_QEMU_ARM_VIRT)
    /* QEMU virt with GICv3 */
    #define GIC_V3_DIST_PADDR       0x08000000
    #define GIC_V3_DIST_SIZE        0x10000
    #define GIC_V3_REDIST_PADDR     0x080A0000
    #define GIC_V3_REDIST_SIZE      0x100000
    #define GIC_V3_REDIST_STRIDE    0x20000
#else
    #error "GICv3 addresses not defined for this platform"
#endif
```

**Test**: Compiles (header-only)

---

### Commit 3: Update build system for GICv3 selection

**Files**: `libsel4vm/CMakeLists.txt`

Update to conditionally compile vgic_v2.c or vgic_v3.c:

```cmake
if(KernelArchARM)
    if(KernelArmGicV3)
        list(APPEND sources src/arch/arm/vgic/vgic_v3.c)
        target_compile_definitions(sel4vm PRIVATE CONFIG_ARM_GIC_V3)
    else()
        list(APPEND sources src/arch/arm/vgic/vgic_v2.c)
    endif()
endif()
```

**Test**: Builds for GICv2 platform (rpi4) - no vgic_v3.c yet, so GICv3 builds will fail until Commit 4

---

### Commit 4: Add vgic_v3.c skeleton with memory reservations

**Files**: `libsel4vm/src/arch/arm/vgic/vgic_v3.c`

Create minimal vgic_v3.c that:
- Includes headers
- Defines vgic_t structure for GICv3
- Implements `vm_install_vgic_v3()` that reserves GICD and GICR memory regions
- Implements stub fault handlers that log and return RAZ/WI
- Implements `vm_vgic_v3_maintenance_handler()` stub

```c
int vm_install_vgic_v3(vm_t *vm) {
    // Allocate vgic state
    // Reserve GICD region with handle_gicd_fault
    // Reserve GICR region with handle_gicr_fault
    // Return 0 on success
}

static memory_fault_result_t handle_gicd_fault(...) {
    ZF_LOGW("GICD access at 0x%lx - stub", paddr);
    if (fault_is_read(vcpu)) {
        fault_set_data(vcpu, 0);  // RAZ
    }
    return advance_fault(vcpu);
}

static memory_fault_result_t handle_gicr_fault(...) {
    ZF_LOGW("GICR access at 0x%lx - stub", paddr);
    if (fault_is_read(vcpu)) {
        fault_set_data(vcpu, 0);  // RAZ
    }
    return advance_fault(vcpu);
}
```

**Test**: Builds for Orin AGX. VM starts but Linux guest hangs at GIC init (expected - all reads return 0).

---

### Commit 5: Implement GICD_CTLR, GICD_TYPER, GICD_IIDR

**Files**: `libsel4vm/src/arch/arm/vgic/vgic_v3.c`

Add distributor state structure and implement key ID registers:

```c
struct vgic_v3_dist {
    uint32_t ctlr;      // Force ARE_NS=1, DS=1
    uint32_t typer;     // Report SPI count, CPU count
    uint32_t iidr;      // Implementer ID
};

// GICD_CTLR: Force ARE_NS=1, DS=1, allow EnableGrp1NS writes
// GICD_TYPER: ITLinesNumber, CPUNumber=0 (use GICR), IDbits=9
// GICD_IIDR: Return fixed implementer ID
```

**Test**: Linux GIC driver reads GICD_TYPER successfully, proceeds to GICR init.

---

### Commit 6: Implement GICR_TYPER and GICR_WAKER

**Files**: `libsel4vm/src/arch/arm/vgic/vgic_v3.c`

Add per-vCPU redistributor state and implement key registers:

```c
struct vgic_v3_redist {
    uint64_t typer;     // Affinity, Last bit, Processor_Number
    uint32_t waker;     // Always 0 (awake)
};

// GICR_TYPER: Construct from vCPU MPIDR, set Last bit for last vCPU
// GICR_WAKER: Always return 0, ignore writes
```

**Test**: Linux GIC driver reads GICR_TYPER, GICR_WAKER and proceeds.

---

### Commit 7: Implement GICD interrupt enable/pending registers

**Files**: `libsel4vm/src/arch/arm/vgic/vgic_v3.c`

Add SPI state arrays and implement enable/pending registers:

```c
struct vgic_v3_dist {
    // ... existing ...
    uint32_t igroupr[32];       // Group (all Group 1 NS)
    uint32_t isenabler[32];     // Enable state
    uint32_t ispendr[32];       // Pending state
    uint32_t ipriorityr[256];   // Priority
    uint32_t icfgr[64];         // Edge/level config
};

// GICD_ISENABLER: Set enable bits
// GICD_ICENABLER: Clear enable bits
// GICD_ISPENDR: Set pending (read returns pending state)
// GICD_ICPENDR: Clear pending
// GICD_IPRIORITYR: Priority per IRQ
// GICD_ICFGR: Edge vs level trigger
// GICD_IGROUPR: All Group 1 NS (read-only 0xFFFFFFFF)
```

**Test**: Linux can enable SPIs. Timer interrupt setup proceeds.

---

### Commit 8: Implement GICR SGI/PPI registers

**Files**: `libsel4vm/src/arch/arm/vgic/vgic_v3.c`

Add SGI/PPI state to redistributor:

```c
struct vgic_v3_redist {
    // ... existing ...
    uint32_t igroupr0;          // Group for SGI/PPI
    uint32_t isenabler0;        // Enable SGI/PPI
    uint32_t ispendr0;          // Pending SGI/PPI
    uint32_t ipriorityr[8];     // Priority for IRQ 0-31
    uint32_t icfgr[2];          // Config for SGI/PPI
};

// GICR_ISENABLER0: Enable SGI/PPI
// GICR_ICENABLER0: Disable SGI/PPI
// GICR_ISPENDR0: Set pending
// GICR_ICPENDR0: Clear pending
// GICR_IPRIORITYR: Priority for IRQ 0-31
// GICR_ICFGR0/1: Edge/level config
```

**Test**: Linux enables timer PPI (27). Timer interrupt pending can be set.

---

### Commit 9: Implement IRQ injection via vm_inject_irq

**Files**: `libsel4vm/src/arch/arm/vgic/vgic_v3.c`

Connect vGIC state to actual interrupt injection:

```c
int vm_inject_irq(vm_vcpu_t *vcpu, int irq) {
    // Check if IRQ is enabled in vgic state
    // If SPI (>=32): check GICD_ISENABLER
    // If PPI/SGI (<32): check GICR_ISENABLER0
    // Call seL4_ARM_VCPU_InjectIRQ()
}

int vm_set_irq_level(vm_vcpu_t *vcpu, int irq, int level) {
    // Update pending state
    // If level && enabled: inject
}
```

**Test**: Timer interrupt delivered to guest. Guest receives and acknowledges IRQ.

---

### Commit 10: Implement maintenance interrupt handler

**Files**: `libsel4vm/src/arch/arm/vgic/vgic_v3.c`

Handle EOI from guest:

```c
int vm_vgic_v3_maintenance_handler(vm_vcpu_t *vcpu) {
    int idx = seL4_GetMR(seL4_VGICMaintenance_IDX);
    // Clear pending state in vgic
    // Call virq_ack() callback if registered
    // Handle level-triggered re-injection
    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
    return VM_EXIT_HANDLED;
}
```

**Test**: Guest can EOI interrupts. Timer works continuously.

---

### Commit 11: Wire up virq registration and callbacks

**Files**: `libsel4vm/src/arch/arm/vgic/vgic_v3.c`

Implement virq_handle management:

```c
int vm_register_irq(vm_vcpu_t *vcpu, int irq, irq_ack_fn_t ack, void *cookie) {
    // Store virq_handle in vgic state
    // For SPIs: store in vgic->vspis[irq - 32]
    // For PPIs: store in vgic->vcpu[n].local_virqs[irq]
}

// Update vm_inject_irq to use virq_handle
// Update maintenance handler to call ack callback
```

**Test**: Physical IRQs can be forwarded to guest with ACK callbacks.

---

### Commit 12: Integration test with Linux guest

**Files**: None (test only)

Test complete flow:
1. Build vm_minimal for Orin AGX
2. Boot Linux guest
3. Verify:
   - GIC driver initializes without errors
   - Timer interrupts work (check /proc/interrupts)
   - Console works (TCU via HSP)

**Test**: Linux boots to shell prompt.

---

### 18.1 Commit Dependency Graph

```
Commit 1 (vgicv3_defs.h)
    │
    ▼
Commit 2 (gicv3.h)
    │
    ▼
Commit 3 (CMakeLists.txt) ◄── Commit 4 depends on 1,2,3
    │
    ▼
Commit 4 (vgic_v3.c skeleton)
    │
    ├──► Commit 5 (GICD_CTLR/TYPER/IIDR)
    │        │
    │        ▼
    │    Commit 7 (GICD enable/pending)
    │        │
    │        ▼
    │    Commit 9 (vm_inject_irq)
    │        │
    │        ▼
    │    Commit 10 (maintenance handler)
    │        │
    │        ▼
    │    Commit 11 (virq registration)
    │
    └──► Commit 6 (GICR_TYPER/WAKER)
             │
             ▼
         Commit 8 (GICR SGI/PPI)
             │
             ▼
         (merges into Commit 9)

Commit 12 (integration test) ◄── All commits complete
```

### 18.2 Estimated Lines of Code per Commit

| Commit | Description | Est. LOC |
|--------|-------------|----------|
| 1 | vgicv3_defs.h | ~80 |
| 2 | gicv3.h | ~25 |
| 3 | CMakeLists.txt | ~10 |
| 4 | vgic_v3.c skeleton | ~150 |
| 5 | GICD ID registers | ~80 |
| 6 | GICR_TYPER/WAKER | ~60 |
| 7 | GICD enable/pending | ~150 |
| 8 | GICR SGI/PPI | ~100 |
| 9 | vm_inject_irq | ~80 |
| 10 | maintenance handler | ~60 |
| 11 | virq registration | ~100 |
| 12 | Integration test | 0 |
| **Total** | | **~895** |

### 18.3 Risk Mitigation

**If Linux hangs during GIC init**:
- Add `ZF_LOGD` in fault handlers to trace register accesses
- Compare access sequence with Linux KVM vgic-mmio-v3.c
- Check GICR_TYPER.Affinity_Value matches VMPIDR_EL2

**If timer interrupt doesn't work**:
- Verify GICR_ISENABLER0 bit 27 is set
- Check maintenance interrupt is being handled
- Verify ICH_LRn_EL2 is being loaded (kernel side)

**If guest hangs after boot**:
- Check for interrupt storms (maintenance not clearing pending)
- Verify virq_ack callbacks are being called

## 19. ICC_SGI*R System Register Trap Handling

### 19.1 Problem Statement

When a VM is idle, Linux sends IPIs (Inter-Processor Interrupts) via SGIs for housekeeping
tasks like RCU callbacks, timer distribution, and scheduler IPIs. These writes to ICC_SGI1R_EL1
trap to EL2 because **SGI generation registers have no ICV counterpart** - they always trap
regardless of HCR_EL2.IMO setting.

**Symptom**: VM crashes with unhandled VCPU fault:
```
======= Unhandled VCPU fault from [vm0] =======
HSR Value: 0x623a3016
HSR Exception Class: HSR_SYSREG_64_EXCEPTION [0x18]
ISS Value: 0x3a3016
```

### 19.2 SGI Register Encodings

| Register | Op0 | Op1 | CRn | CRm | Op2 | Purpose |
|----------|-----|-----|-----|-----|-----|---------|
| ICC_SGI1R_EL1 | 3 | 0 | 12 | 11 | 5 | Generate Group 1 SGI |
| ICC_SGI0R_EL1 | 3 | 0 | 12 | 11 | 7 | Generate Group 0 SGI |
| ICC_ASGI1R_EL1 | 3 | 0 | 12 | 11 | 6 | Generate Group 1 SGI (alias) |

### 19.3 ICC_SGI1R_EL1 Register Format

```
63                   56 55      48 47      40 39  32
+---------------------+-----------+-----------+-----+
|       Aff3          |   (res0)  |   Aff2    | IRM |
+---------------------+-----------+-----------+-----+

31      24 23  16 15                              0
+---------+------+-----------------------------------+
|  INTID  | Aff1 |           TargetList              |
+---------+------+-----------------------------------+
```

- **Aff3/Aff2/Aff1** [55:48, 39:32, 23:16]: Affinity levels of target PEs
- **IRM** [40]: Interrupt Routing Mode
  - 0: Use TargetList to select target PEs
  - 1: Route to all PEs except self
- **INTID** [27:24]: SGI number (0-15)
- **TargetList** [15:0]: Bitmap of target PEs at specified affinity

### 19.4 Implementation: Single-vCPU (Phase 1)

For single-vCPU VMs, SGI handling is simple:
- Any SGI is a self-SGI (only one CPU)
- Just inject the SGI back to the same vCPU

**Location**: `libsel4vmmplatsupport/src/sel4_arch/aarch64/sysreg_exception.c`

```c
/* ICC_SGI1R_EL1: Op0=3, Op1=0, CRn=12, CRm=11, Op2=5 */
#define ICC_SGI1R_OP0   3
#define ICC_SGI1R_OP1   0
#define ICC_SGI1R_CRN   12
#define ICC_SGI1R_CRM   11
#define ICC_SGI1R_OP2   5

/* Similarly for ICC_SGI0R_EL1 (Op2=7) and ICC_ASGI1R_EL1 (Op2=6) */

static int handle_icc_sgi_exception(vm_vcpu_t *vcpu, sysreg_t *sysreg, bool is_read)
{
    if (is_read) {
        /* SGI registers are write-only - reads are UNDEFINED */
        ZF_LOGW("ICC_SGI*R read (undefined behavior)");
        advance_vcpu_fault(vcpu);
        return 0;
    }

    /* Get the value being written from guest register Rt */
    seL4_UserContext regs;
    int err = vm_get_thread_context(vcpu, &regs);
    if (err) {
        ZF_LOGE("Failed to get thread context");
        return -1;
    }

    uint64_t sgi_value = get_reg_from_context(&regs, sysreg->params.rt);

    /* Extract SGI number (INTID field, bits 27:24) */
    int sgi_intid = (sgi_value >> 24) & 0xF;

    ZF_LOGI("ICC_SGI*R write: INTID=%d IRM=%d Aff=0x%x TargetList=0x%x",
            sgi_intid,
            (int)((sgi_value >> 40) & 1),
            (int)((sgi_value >> 16) & 0xFF),
            (int)(sgi_value & 0xFFFF));

    /* For single-vCPU: inject SGI to self */
    vm_inject_irq(vcpu, sgi_intid);

    advance_vcpu_fault(vcpu);
    return 0;
}
```

### 19.5 Implementation: Multi-vCPU (Phase 2)

For multi-vCPU VMs:
1. Parse IRM bit to determine routing mode
2. If IRM=1: inject to all vCPUs except self
3. If IRM=0: parse Aff3/Aff2/Aff1 and TargetList to find target vCPUs
4. Match affinity against VMPIDR_EL2 of each vCPU
5. Inject SGI to each matching vCPU

### 19.6 Testing

**Test case**: Boot Linux VM, let it go idle for 30+ seconds
- **Before fix**: VM crashes with HSR_SYSREG_64_EXCEPTION
- **After fix**: VM remains running, console responsive

**Debug verification**:
```bash
# In guest Linux
cat /proc/interrupts | grep IPI
# Should show IPI counts incrementing
```

### 19.7 Commit: Add ICC_SGI*R trap handler

**Files**: `libsel4vmmplatsupport/src/sel4_arch/aarch64/sysreg_exception.c`

Add to `sysreg_table[]`:
```c
/* ICC_SGI1R_EL1: Generate Group 1 SGI */
{
    .sysreg = { .params.op0 = 3, .params.op1 = 0, .params.op2 = 5,
                .params.crn = 12, .params.crm = 11 },
    .sysreg_match_mask = { .hsr_val = SYSREG_MATCH_ALL_MASK },
    .handler = handle_icc_sgi_exception
},
/* ICC_ASGI1R_EL1: Generate Group 1 SGI (alias) */
{
    .sysreg = { .params.op0 = 3, .params.op1 = 0, .params.op2 = 6,
                .params.crn = 12, .params.crm = 11 },
    .sysreg_match_mask = { .hsr_val = SYSREG_MATCH_ALL_MASK },
    .handler = handle_icc_sgi_exception
},
/* ICC_SGI0R_EL1: Generate Group 0 SGI */
{
    .sysreg = { .params.op0 = 3, .params.op1 = 0, .params.op2 = 7,
                .params.crn = 12, .params.crm = 11 },
    .sysreg_match_mask = { .hsr_val = SYSREG_MATCH_ALL_MASK },
    .handler = handle_icc_sgi_exception
},
```

**Test**: Linux VM survives idle period without crashing.

**Estimated LOC**: ~60

## 20. BPMP Driver Bug Investigation and vGIC Fixes (2026-01)

### 20.1 Problem Statement

**Symptom**: BPMP (Boot and Power Management Processor) driver in guest Linux on Orin AGX gets stuck during probe. Adding even slight delays in the IRQ path in guest Linux makes BPMP work.

**Root Cause**: Multiple bugs in GICv3 vGIC emulation causing race conditions and incorrect interrupt handling. The delays work around:
1. Asynchronous injection window - guest doesn't see injected IRQ immediately
2. ACK callback serialization - delay ensures one IRQ's ACK completes before next
3. Queue overflow prevention - the 64-entry IRQ queue can overflow with rapid-fire IRQs
4. BPMP protocol timing - delay ensures request-response pairs complete before new IRQs

### 20.2 Identified Bugs

Nine bugs were identified through code review and exploration:

| Issue | Severity | Location | Description |
|-------|----------|----------|-------------|
| 1 | **CRITICAL** | vgic_v3.c:929-953 | Missing `lr_shadow` update for direct injection path |
| 2 | **CRITICAL** | vgic_v3.c:895-898 | Missing `seL4_Reply` on maintenance error → vCPU hangs |
| 3 | Medium | vgic_v3.c:542-640 | Missing GICR pending/active register emulation |
| 4 | Medium | vgic_v3.c:968-976 | SGI/PPI pending not tracked when disabled |
| 5 | Medium | vgic_v3.c | Missing pending injection on GICR_ISENABLER0 write |
| 6 | **CRITICAL** | vgic_v3.c:944 | Priority hardcoded to 0 (highest!) for unregistered IRQs |
| 7 | Low | vgic_v3.c:944 | Group hardcoded to 1, no GICD_CTLR.EnableGrp1NS check |
| 8 | Low | vgic_v3.c | No ISB memory barriers after GICD/GICR register writes |
| 9 | Medium | virq.h | No backpressure - IRQs silently dropped on queue overflow |

### 20.3 Issue Details

#### Issue 1: Missing lr_shadow Update (CRITICAL)

**Problem**: When unregistered IRQs (like IRQ 208 from BPMP) are injected directly via `seL4_ARM_VCPU_InjectIRQ()`, the `lr_shadow` array is NOT updated. When the guest EOIs the IRQ, the maintenance handler finds `lr_shadow[idx] == NULL` and returns early without calling the ACK callback. The physical IRQ stays masked forever.

**Code location**: `vgic_v3.c` lines 929-953

```c
if (!virq_data) {
    DVGIC("IRQ %d not registered, injecting directly", irq);
    idx = vgic_find_empty_list_reg(vgic, vcpu);
    // ...
    int err = seL4_ARM_VCPU_InjectIRQ(vcpu->vcpu.cptr, irq, 0, 1, idx);
    // BUG: lr_shadow[idx] not updated! Maintenance handler won't process it
    return 0;
}
```

**Fix**: Add `lr_irq_num[]` array to track IRQ number in each list register. Update maintenance handler to process direct-injected IRQs.

#### Issue 2: Missing seL4_Reply on Error (CRITICAL)

**Problem**: When `handle_vgic_maintenance()` returns an error, `vm_vgic_maintenance_handler()` logs the error but does NOT send `seL4_Reply()`. The vCPU hangs indefinitely waiting for a reply.

**Code location**: `vgic_v3.c` lines 895-898

```c
int err = handle_vgic_maintenance(vcpu, idx);
if (err) {
    ZF_LOGE("vGIC maintenance handler failed (error %d)", err);
    // BUG: No seL4_Reply() - vCPU hangs!
}
```

**Fix**: Always call `seL4_Reply()` regardless of error status.

#### Issue 3: Missing GICR Pending/Active Registers

**Problem**: `GICR_ISPENDR0`, `GICR_ICPENDR0`, `GICR_ISACTIVER0`, `GICR_ICACTIVER0` are not emulated. The `gicv3_redist_state` struct lacks `ispendr0` and `isactiver0` fields.

**Fix**: Add missing fields to struct and implement read/write handlers.

#### Issue 4: SGI/PPI Pending Not Tracked

**Problem**: When `vm_inject_irq()` is called for a disabled IRQ, only SPIs (irq >= 32) get their pending bit set. SGIs/PPIs (irq < 32) are silently dropped.

**Code location**: `vgic_v3.c` lines 968-976

**Fix**: Track pending state in `GICR` `ispendr0` for SGI/PPIs.

#### Issue 5: Missing Pending Injection on GICR_ISENABLER0 Write

**Problem**: Unlike `GICD_ISENABLER` handler, `GICR_ISENABLER0` handler doesn't check for pending SGI/PPIs to inject when the interrupt is enabled.

**Fix**: When `GICR_ISENABLER0` is written, check `ispendr0` for newly-enabled IRQs and inject them.

#### Issue 6: Priority Hardcoded to 0 (CRITICAL)

**Problem**: Direct injection path uses `priority=0` (the HIGHEST priority in GICv3):

```c
int err = seL4_ARM_VCPU_InjectIRQ(vcpu->vcpu.cptr, irq, 0, 1, idx);
//                                                    ↑
//                                              priority=0 (HIGHEST!)
```

This causes IRQ 208 (BPMP) to preempt everything, potentially causing priority inversion.

**Fix**: Use proper priority from `GICD_IPRIORITYR` or default `0xA0` (GIC_PRI_IRQ).

#### Issue 7: Group Hardcoded to 1

**Problem**: Group 1 (non-secure) is hardcoded for all injections. Code doesn't check if `GICD_CTLR.EnableGrp1NS` is set. If guest hasn't enabled group 1 interrupts, they're silently ignored.

**Fix**: Check `GICD_CTLR.EnableGrp1NS` before injection; queue as pending if not enabled.

#### Issue 8: No Memory Barriers

**Problem**: No `isb()` after GICD/GICR emulated register writes. On ARM64 with speculative execution, this can cause stale register values.

**Fix**: Add `asm volatile("isb" ::: "memory")` before returning from fault handlers.

#### Issue 9: IRQs Silently Dropped on Queue Overflow

**Problem**: The 64-entry IRQ queue (`MAX_IRQ_QUEUE_LEN`) can overflow. When it does, `vgic_irq_enqueue()` returns -1 and the IRQ is silently dropped with just a warning log.

**Fix**: Set pending bit so IRQ can be re-attempted when queue has space.

### 20.4 Implementation Plan

#### Phase 1: Critical Fixes (Unblock BPMP)

| Fix | Description | Status |
|-----|-------------|--------|
| 1B | Always send `seL4_Reply()` | ✅ DONE |
| 1A | Track direct-injected IRQs with `lr_irq_num[]` | ✅ DONE |
| 3A | Use proper priority (0xA0 instead of 0) | ✅ DONE |

#### Phase 2: GICR State Tracking

| Fix | Description | Status |
|-----|-------------|--------|
| 2A | Add `ispendr0`, `isactiver0` to `gicv3_redist_state` | TODO |
| 2B | Handle GICR pending/active register reads | TODO |
| 2C | Handle GICR pending/active register writes | TODO |
| 2D | Track SGI/PPI pending when disabled | TODO |
| 2E | Inject pending on GICR_ISENABLER0 write | TODO |

#### Phase 3: Robustness Improvements

| Fix | Description | Status |
|-----|-------------|--------|
| 3B | Check GICD_CTLR.EnableGrp1NS before injection | TODO |
| 4A | Add ISB memory barriers after register writes | TODO |
| 4B | Improve queue overflow handling | TODO |

### 20.5 Files to Modify

1. **`projects/sel4_projects_libs/libsel4vm/src/arch/arm/vgic/vgic_v3.c`**
   - `struct gicv3_redist_state` - Add `ispendr0`, `isactiver0`
   - `vm_inject_irq()` - Track direct-injected IRQs, track SGI/PPI pending, use proper priority
   - `handle_vgic_maintenance()` - Handle direct-injected IRQs
   - `vm_vgic_maintenance_handler()` - Always send seL4_Reply
   - `gicr_sgi_read_reg()` - Add ISPENDR0, ICPENDR0, ISACTIVER0, ICACTIVER0 cases
   - `gicr_sgi_write_reg()` - Add ISPENDR0, ICPENDR0, ISACTIVER0, ICACTIVER0 cases
   - Fault handlers - Add ISB before return

2. **`projects/sel4_projects_libs/libsel4vm/src/arch/arm/vgic/virq.h`**
   - `vgic_vcpu_t` - Add `lr_irq_num[NUM_LIST_REGS]` array
   - `vgic_vcpu_load_list_reg()` - Use priority from IRQ config
   - `vgic_irq_enqueue()` - Improve overflow handling

### 20.6 Testing Plan

1. **Build sel4test** (sanity check):
   ```
   make mrproper
   make orinagx_defconfig
   make sel4test
   mcp__sel4-autopilot__test_sel4_efi(..., profile="sel4test")
   ```

2. **Build and test vm_minimal** (BPMP test):
   ```
   make mrproper
   make orinagx_defconfig
   make vm_minimal
   mcp__sel4-autopilot__test_sel4_efi(..., profile="vm-minimal")
   ```

3. **Verify BPMP initialization** in profile-defined console logs under `results/<id>/console/`:
   - Look for `tegra-bpmp` probe success
   - No "timeout" or "stuck" messages
   - Clock/reset/power domain operations complete

### 20.7 Progress Log

| Date | Work Done | Result |
|------|-----------|--------|
| 2026-01-18 | Initial investigation, identified 9 bugs | Plan created |
| 2026-01-18 | Implemented critical fixes 1A, 1B, 3A | Committed to sel4_projects_libs virtioso-next |
| 2026-01-18 | Fixed priority 0xa0 -> 16 (seL4 uses 5-bit priority 0-31) | Priority was causing seL4_RangeError |
| 2026-01-18 | Added debug tracing for IRQ 208 and maintenance | Confirmed IRQ delivery IS working |
| 2026-01-18 | **Finding**: Physical IRQ 208 delivered OK, guest EOIs OK | vGIC fixes are correct |
| 2026-01-18 | **Finding**: Multiple rapid IRQ 208s during BPMP probe | Doorbell keeps firing |

### 20.8 Current Investigation Status

**vGIC fixes appear correct - problem may be elsewhere**

The debug output shows:
1. Physical IRQ 208 is being delivered to VMM via irq_server
2. vm_inject_irq loads IRQ 208 to LR0 correctly
3. Guest receives the interrupt (maintenance fires)
4. Maintenance handler calls virq_ack, which acks physical IRQ
5. Pattern repeats with multiple rapid IRQ 208s

**Observed pattern during BPMP probe:**
```
irq_handler: received physical IRQ 208
vm_inject_irq(208): LR state: [0, 0, 0, 0]
vm_inject_irq: loaded IRQ 208 to LR0
MAINTENANCE: calling virq_ack for IRQ 208
do_irq_server_ack: IRQ 208 EOI
[repeats 6+ times in rapid succession]
```

**Possible root causes (not vGIC):**
1. **HSP doorbell stays asserted**: If physical doorbell level not cleared, IRQ re-fires
2. **Mailbox read timing**: Guest may not read mailbox before ACK clears interrupt
3. **HSP register passthrough issue**: MMIO to HSP might not reach real hardware
4. **BPMP IPC protocol issue**: Response handling in driver might be broken

**Next steps:**
- Check if HSP doorbell passthrough is working correctly
- Add debug to HSP MMIO accesses
- Verify guest is reading/writing HSP registers correctly
