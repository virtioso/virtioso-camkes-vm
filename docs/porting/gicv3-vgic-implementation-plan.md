# GICv3 Virtual GIC Implementation Plan

## Overview

The seL4 VMM currently only supports GICv2 virtual interrupt controller emulation. Platforms with GICv3 (like NVIDIA Orin AGX/Tegra234) cannot run guest VMs until GICv3 vGIC support is implemented.

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

## Conclusion

GICv3 vGIC support is more tractable than initially expected:

**Good news:**
- seL4 kernel already has ICH_* (hypervisor GIC) register access for virtual IRQ injection
- VCPU fault forwarding already works - VMM receives full ESR for system register traps
- Only ~15 lines of kernel code needed to enable ICC_* trapping (ICC_SRE_EL2 configuration)

**Remaining work:**
- New GICR (Redistributor) emulation in VMM
- ICC_* register emulation in VMM (decode ESR, handle read/write)
- GICD updates for affinity routing (GICD_IROUTER instead of GICD_ITARGETSR)

The Linux KVM and Xen implementations can serve as references. The seL4 kernel's existing GICv3 support significantly reduces the scope of required kernel changes.
