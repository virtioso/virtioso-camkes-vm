# ARM64 Memory Management Register Audit for seL4 on Orin AGX

## Document Purpose

This document captures findings from a systematic audit of ARM64 memory management registers used by seL4 on NVIDIA Orin AGX. The audit was initiated to investigate systematic test failures (page faults, permission errors) that persisted after fixing a TLB-related RAS crash.

---

## ⚠️ CRITICAL REQUIREMENTS (NON-NEGOTIABLE)

### 1. RAS Errors are SOFTWARE BUGS

**RAS (Reliability, Availability, Serviceability) errors on NVIDIA Tegra are SOFTWARE BUGS in seL4, NOT hardware problems.**

- RAS errors indicate seL4 is generating accesses to PA=0x0 (NULL pointer access)
- **Note**: RAS error ADDR field uses ARM standard encoding where bit 63 = NS flag, not part of address. See `orin-ras-error-investigation.md` for details.
- They must be investigated and eliminated at the source
- `KernelAArch64SErrorIgnore ON` is a **workaround**, not a solution
- Tests "passing" while ignoring SErrors is NOT a valid pass

### 2. Must Work WITHOUT SError Ignore

**The goal is to run sel4test with `KernelAArch64SErrorIgnore OFF` and have zero SErrors.**

- Current config uses `KernelAArch64SErrorIgnore ON` to hide bugs
- This must be changed to OFF once root causes are fixed
- Any SError with ignore OFF will halt the kernel - this is correct behavior
- Success = all tests pass with SError ignore disabled

### 3. Read and Prefetch Faults are NOT Acceptable

**All page faults in sel4test indicate real bugs that must be fixed:**

- Instruction abort / Translation fault = Bug (page should be mapped)
- Instruction abort / Address size fault = Bug (PA size mismatch)
- Data abort / Permission fault = Bug (page permissions wrong)
- Prefetch faults = Bug (MMU configuration or page table error)

**These are NOT "intermittent issues" or "test framework bugs" - they are real problems in the seL4 port that must be eliminated.**

---

## Executive Summary

### Key Finding: Exception Level Architecture

In seL4 with `CONFIG_ARM_HYPERVISOR_SUPPORT` enabled:

| Exception Level | What Runs Here | Translation Mechanism |
|-----------------|----------------|----------------------|
| **EL0** | Native userspace threads | Stage-2 via VTTBR_EL2 |
| **EL1** | Guest OS (vCPU only) | Stage-1 + Stage-2 |
| **EL2** | seL4 kernel | Stage-1 via TTBR0_EL2 |

**Critical insight**: For sel4test (which runs native seL4 threads, NOT VMs), EL1 registers (TCR_EL1, TTBR0_EL1, MAIR_EL1) being zero is **expected and correct** - they are only configured when running vCPUs.

---

## Register Categories

### 1. EL2 Registers (Hypervisor/Kernel)

These are the primary registers for seL4 in hypervisor mode:

#### TTBR0_EL2 - Translation Table Base Register 0 (EL2)
- **Purpose**: Stage-1 translation for kernel (EL2) memory accesses
- **Set by**: `setCurrentKernelVSpaceRoot()` in `kernel/include/arch/arm/arch/64/mode/machine.h:164-177`
- **Value at boot**: Points to `armKSGlobalKernelPGD`
- **Observed value**: `0x00000008bf26b000` (valid kernel page table)

#### VTTBR_EL2 - Virtualization Translation Table Base Register
- **Purpose**: Stage-2 translation for EL0/EL1 memory accesses
- **Format**: `[63:48] = VMID, [47:0] = page table base address`
- **Set by**: `setCurrentUserVSpaceRoot()` in `kernel/include/arch/arm/arch/64/mode/machine.h:179-188`
- **Context switch**: Updated per-thread via `armv_contextSwitch()` in `kernel/include/arch/arm/armv/armv8-a/64/armv/context_switch.h`
- **Observed value**: `0x00000008bf267000` (VMID=0, valid base)

**TLB Fix Applied**: The TLB invalidation code now uses `armKSGlobalUserVSpace` as a valid page table base instead of NULL (base=0), which was causing RAS errors on Tegra234 due to speculative table walks.

#### TCR_EL2 - Translation Control Register (EL2)
- **Purpose**: Controls stage-1 translation for EL2
- **Set by**: Elfloader (NOT kernel!)
- **Expected value**: `0x80823510` (TCR_EL2_DEFAULT)
- **Observed value**: `0x0000000080823510` (matches expected)
- **Validation**: `checkTCR_EL2()` in `kernel/include/arch/arm/arch/64/mode/machine.h:156-162`

TCR_EL2 bit breakdown:
```
TCR_EL2_DEFAULT = TCR_EL2_T0SZ(16)      | // 48-bit VA
                  TCR_EL2_IRGN0_WBWC    | // Inner write-back, write-allocate
                  TCR_EL2_ORGN0_WBWC    | // Outer write-back, write-allocate
                  TCR_EL2_SH0_ISH       | // Inner shareable
                  TCR_EL2_TG0_4K        | // 4KB granule
                  (TCR_EL2_TCR_PS << 16)| // Physical address size
                  TCR_EL2_RES1            // Reserved bits
```

#### VTCR_EL2 - Virtualization Translation Control Register
- **Purpose**: Controls stage-2 translation (for EL0/EL1 accesses)
- **Set by**: `vcpu_init_vtcr()` in `kernel/include/arch/arm/armv/armv8-a/64/armv/vcpu.h:575-612`
- **Called from**: `vcpu_boot_init()` -> `armv_vcpu_boot_init()` during kernel boot
- **Observed value**: `0x00000000800a3558`

Configuration (default, non-40-bit PA):
```c
vtcr_el2 = VTCR_EL2_T0SZ(20);           // 44-bit input IPA
vtcr_el2 |= VTCR_EL2_PS(PS_16T);        // 44-bit PA size
vtcr_el2 |= VTCR_EL2_SL0(SL0_4K_L0);    // 4KiB, start at level 0
vtcr_el2 |= VTCR_EL2_IRGN0(NORMAL_WB_WA_CACHEABLE);
vtcr_el2 |= VTCR_EL2_ORGN0(NORMAL_WB_WA_CACHEABLE);
vtcr_el2 |= VTCR_EL2_SH0(SH0_INNER);    // Inner shareable
vtcr_el2 |= VTCR_EL2_TG0(TG0_4K);       // 4KiB page size
vtcr_el2 |= BIT(31);                     // Reserved as 1
```

#### HCR_EL2 - Hypervisor Configuration Register
- **Purpose**: Controls hypervisor behavior, enables stage-2 translation
- **Set by**: `armv_vcpu_boot_init()` sets `HCR_NATIVE` for native threads
- **Observed value**: `0x00000000fe08703b`

HCR_NATIVE definition:
```c
#define HCR_COMMON ( HCR_VM | HCR_RW | HCR_AMO | HCR_IMO | HCR_FMO | HCR_TSC)
#define HCR_NATIVE ( HCR_COMMON | HCR_TGE | HCR_TVM | HCR_TTLB | HCR_DC \
                   | HCR_TAC | HCR_SWIO | HCR_TSC )
```

Key bits:
- `HCR_VM` (bit 0): Virtualization MMU enable - **enables stage-2 translation**
- `HCR_TGE` (bit 27): Trap general exceptions - routes EL0 exceptions to EL2
- `HCR_DC` (bit 12): Default cacheable - **disables stage-1 translation at EL1**
- `HCR_RW` (bit 31): EL1 is AArch64

#### SCTLR_EL2 - System Control Register (EL2)
- **Purpose**: Controls caches, MMU, alignment for EL2
- **Set by**: `kernel/src/arch/arm/64/head.S` (CR_BITS_SET/CR_BITS_CLEAR)
- **Observed value**: `0x0000000030c5183d`

#### MAIR_EL2 - Memory Attribute Indirection Register (EL2)
- **Purpose**: Defines memory types for stage-1 translation at EL2
- **Set by**: Elfloader
- **Observed value**: `0x00000000000044ff`

MAIR indices used by seL4:
```c
// From kernel/src/arch/arm/64/kernel/vspace.c
#define MAIR_ATTR(attr, mt) (((attr) & 0xff) << ((mt) * 8))
// Index 0: Device-nGnRnE (0x00)
// Index 1: Normal, Write-Back (0x44 or 0xff depending on config)
```

---

### 2. EL1 Registers (Guest OS / vCPU)

**For sel4test, these are expected to be zero or undefined** since no VMs are running.

#### TCR_EL1 - Translation Control Register (EL1)
- **Observed value**: `0x0000000000000000`
- **Status**: Expected for native-only workloads

#### TTBR0_EL1 / TTBR1_EL1 - Translation Table Base Registers (EL1)
- **Observed values**: Both `0x0000000000000000`
- **Status**: Expected for native-only workloads

#### MAIR_EL1 - Memory Attribute Indirection Register (EL1)
- **Observed value**: `0x0000000000000000`
- **Status**: Expected for native-only workloads

#### SCTLR_EL1 - System Control Register (EL1)
- **Observed value**: `0x0000000000c50838`
- **Set by**: `vcpu_enable()` / `vcpu_disable()` for vCPU threads
- **Note**: Non-zero because default bits are set, but not actively used

---

## Address Translation Flow

### Native Thread (EL0) Memory Access

```
EL0 Virtual Address
        |
        v
   [Stage-2 Only - HCR_EL2.DC=1 disables Stage-1]
        |
        v
   VTTBR_EL2 (page table base + VMID)
        |
        v
   VTCR_EL2 (translation control)
        |
        v
Physical Address
```

### Kernel (EL2) Memory Access

```
EL2 Virtual Address
        |
        v
   [Stage-1 Only]
        |
        v
   TTBR0_EL2 (page table base)
        |
        v
   TCR_EL2 (translation control)
        |
        v
Physical Address
```

---

## Context Switch Register Updates

When switching between threads, `armv_contextSwitch()` updates:

1. **VTTBR_EL2**: New page table base + VMID for the thread
2. **TLB invalidation**: `invalidateLocalTLB_VMALLS12E1()` if VMID changes

File: `kernel/include/arch/arm/armv/armv8-a/64/armv/context_switch.h`

```c
static inline void armv_contextSwitch(vspace_root_t *vspace, asid_t asid)
{
#ifdef CONFIG_ARM_HYPERVISOR_SUPPORT
    asid = getHWASID(asid);  // Convert to hardware VMID
#endif
    setCurrentUserVSpaceRoot(ttbr_new(asid, pptr_to_paddr(vspace)));
}
```

---

## Boot-Time Register Initialization

### Elfloader Responsibilities
- Sets TCR_EL2, MAIR_EL2, SCTLR_EL2
- Kernel validates TCR_EL2 but does NOT set it

### Kernel Boot Sequence (`init_cpu()`)

1. `activate_kernel_vspace()` - Sets TTBR0_EL2 and VTTBR_EL2
2. `vcpu_boot_init()` - Sets VTCR_EL2 and HCR_EL2

File: `kernel/src/arch/arm/kernel/boot.c:180-195`

---

## TLB Operations

### TLB Invalidation Functions

| Function | Purpose | TLBI Instruction |
|----------|---------|------------------|
| `invalidateLocalTLB_EL2()` | Invalidate EL2 TLB | `tlbi alle2` |
| `invalidateLocalTLB_EL1()` | Invalidate EL1 TLB | `tlbi alle1` |
| `invalidateLocalTLB_VMALLS12E1()` | Invalidate stage 1+2 for current VMID | `tlbi vmalls12e1` |
| `invalidateLocalTLB_VMID()` | Invalidate for specific VMID | Switch VTTBR, `tlbi vmalls12e1` |
| `invalidateLocalTLB_IPA()` | Invalidate by IPA | `tlbi ipas2e1` + `tlbi vmalle1` |

### TLB Fix (commit 448255694ff7)

The fix changed `invalidateLocalTLB_VMID()` to use `armKSGlobalUserVSpace` as the page table base when switching VMIDs for TLB invalidation:

```c
// Before (caused RAS errors on Tegra234):
setCurrentUserVSpaceRoot(ttbr_new(vmid, 0));

// After (fixed):
setCurrentUserVSpaceRoot(ttbr_new(vmid, addrFromKPPtr(armKSGlobalUserVSpace)));
```

File: `kernel/include/arch/arm/armv/armv8-a/64/armv/tlb.h`

---

## Observed Register Values at Boot

From test run with debug output:

```
=== MM Register Dump (kernel boot) EL2 ===
SCTLR_EL2:  0x0000000030c5183d
TCR_EL2:    0x0000000080823510 (expected: 0x0000000080823510)
TTBR0_EL2:  0x00000008bf26b000
VTTBR_EL2:  0x00000008bf267000 (VMID=0, base=0x8bf267000)
VTCR_EL2:   0x00000000800a3558
HCR_EL2:    0x00000000fe08703b
MAIR_EL2:   0x00000000000044ff
SCTLR_EL1:  0x0000000000c50838
TCR_EL1:    0x0000000000000000
TTBR0_EL1:  0x0000000000000000
TTBR1_EL1:  0x0000000000000000
MAIR_EL1:   0x0000000000000000
=== End MM Register Dump ===
```

**Analysis**: All EL2 registers are correctly configured. EL1 registers being zero is expected for sel4test.

---

## Test Failure Analysis

### Fault Types Observed

From sel4test runs, two main fault types occur:

| FSR Code | Meaning | Count |
|----------|---------|-------|
| 0x82000007 | Instruction Abort, Translation fault level 3 | Many |
| 0x82000003 | Instruction Abort, Address size fault level 3 | Many |
| 0x93c08047 | Data Abort (write), Translation fault level 3 | Some |
| 0x93df8047 | Data Abort (write), Translation fault level 3 | Some |

### ESR/FSR Decoding

ARM64 ESR_EL2 format:
- Bits [31:26] = EC (Exception Class)
- Bit [25] = IL (Instruction Length)
- Bits [24:0] = ISS (Instruction Specific Syndrome)

For Data/Instruction Abort:
- ISS bits [5:0] = DFSC/IFSC (Fault Status Code)
- ISS bit [6] = WnR (Write not Read, for data aborts)

Fault Status Codes:
```
0x03 = Address size fault, level 3  (address exceeds PA size)
0x07 = Translation fault, level 3   (page not mapped)
0x0f = Permission fault, level 3    (access not allowed)
```

### Critical Pattern: Recurring Fault Address

Many tests fail at address **0x4001ec** which is:
- `ret` instruction in `deregister_tm_clones` (C runtime)
- Part of standard code that should always be mapped
- Page: 0x400000-0x401000 (first page of code section)

The non-deterministic alternation between:
- "Address size fault" (0x03) - suggests corrupted PTE with invalid PA
- "Translation fault" (0x07) - suggests missing PTE

This pattern indicates either:
1. Race condition in page table setup
2. Page table corruption during execution
3. TLB coherency issue

### VTCR_EL2 Configuration

Observed value: `0x800a3558`

Decoded:
- T0SZ = 24 → 40-bit IPA space (2^40 = 1TB)
- PS = 2 → 40-bit PA (1TB)
- SL0 = 1 → Start at level 1
- TG0 = 0 → 4KB granule

This matches `CONFIG_ARM_PA_SIZE_BITS_40` which is correct for Orin AGX (memory range 0x80000000-0x280000000 requires only ~34 bits).

---

## Outstanding Issues - MUST FIX

### RAS Errors Still Occurring (SOFTWARE BUG - MUST ELIMINATE)

Despite the TLB fix, RAS errors still occur:
```
RAS Error in IPC1003: address = 0x8000000000000040
```

**THIS IS A SOFTWARE BUG, NOT A HARDWARE PROBLEM.**

**ADDR field decoding** (see ARM RAS ERR<n>ADDR encoding):
- `0x8000000000000040`: bit 63 (NS) = 1, bits 55:0 (PA) = `0x40`
- This means: **access to PA=0x40 from non-secure world**, NOT "invalid address with bit 63 set"

**Hypothesized cause** (see `orin-ras-error-investigation.md`):
1. VTTBR_EL2 set with NULL base during ASID/TLB operations
2. Speculative table walks attempt to read from PA near 0x0
3. Tegra234's memory subsystem generates RAS errors for accesses to unmapped low addresses

**CURRENT STATUS**: seL4 ignores SErrors (`KernelAArch64SErrorIgnore ON`). This is a **WORKAROUND, NOT A FIX**. The root cause must be found and eliminated.

**REQUIRED ACTION**:
- Audit all VTTBR_EL2 writes to ensure base address is never NULL
- Verify TLB invalidation sequences use valid page table bases
- Check for race conditions in VMID/ASID switching
- See `orin-ras-error-investigation.md` for detailed investigation

### CRITICAL BUG FOUND: Page Table Level Mismatch

**Root Cause**: CMake configuration bug in `kernel/src/arch/arm/config.cmake` line 106.

The bug:
```cmake
# WRONG - ARM_HYPERVISOR_SUPPORT is C define name, not CMake variable
if(KernelArmPASizeBits40 AND ARM_HYPERVISOR_SUPPORT)

# CORRECT - KernelArmHypervisorSupport is the CMake variable
if(KernelArmPASizeBits40 AND KernelArmHypervisorSupport)
```

**Impact**: This caused `AARCH64_VSPACE_S2_START_L1` to always be OFF, even when both `CONFIG_ARM_PA_SIZE_BITS_40` and `CONFIG_ARM_HYPERVISOR_SUPPORT` are enabled.

**Result**: Hardware/software mismatch:
- **VTCR_EL2** (hardware): SL0=1, 3-level page tables starting at L1
- **Kernel software**: UPT_LEVELS=4, creating 4-level page tables starting at L0

This mismatch causes the MMU to misinterpret page table entries, leading to:
- Translation faults (page table structure mismatch)
- Address size faults (interpreting L0 table as L1 block descriptor)

**Fix Applied**: Changed `ARM_HYPERVISOR_SUPPORT` to `KernelArmHypervisorSupport` in the CMake condition.

### Fix Verification Results (2025-12-13)

After applying the fix and rebuilding:

1. **Config correctly set**: `CONFIG_AARCH64_VSPACE_S2_START_L1 = 1` in `gen_config.h`

2. **VTCR_EL2 verified**: Value `0x80023558` shows:
   - T0SZ = 24 → 40-bit IPA space
   - PS = 2 → 40-bit PA (1TB)
   - **SL0 = 1** → Start at level 1 (3-level page tables)
   - TG0 = 0 → 4KB granule

3. **Multi-run test results (3 runs)**:
   - All 3 runs completed successfully (no boot crashes)
   - Many tests now pass: BIND*, BREAKPOINT_001/002/005, CACHEFLUSH0004, CNODEOP*, etc.
   - No more "address size fault level 3" errors from the page table mismatch
   - Some remaining failures are due to different issues (pre-existing test bugs, RAS errors)

4. **Boot register dump confirms correct setup**:
   ```
   TCR_EL2:    0x0000000080823510 (expected: 0x0000000080823510) ✓
   VTCR_EL2:   0x0000000080023558 (SL0=1 confirms 3-level tables) ✓
   ```

### Remaining Issues (Post-Fix) - ALL ARE BUGS TO FIX

1. **RAS errors**: SOFTWARE BUG - accesses to PA near 0x0, likely due to NULL VTTBR_EL2 base (see `orin-ras-error-investigation.md`)
2. **Translation faults (level 3)**: SOFTWARE BUG - pages not correctly mapped (`FSR 0x82000007`)
3. **Prefetch/read faults**: SOFTWARE BUG - MMU configuration or page table errors

**None of these are acceptable. All must be eliminated.**

### Active Investigation Areas

1. **Page table entry construction**: Verify all PTEs have correct format for stage-2
2. **TLB invalidation sequences**: Verify all barriers are correct per ARM spec
3. **Context switch race conditions**: Check for missing synchronization
4. **Tegra-specific ACTLR/errata**: May need platform-specific configuration
5. **ATF register dumps**: Enhance ATF to dump full state on SError for debugging

### ATF Debug Enhancement - IMPLEMENTED

**File**: `Linux_for_Tegra/source/tegra/optee-src/atf/arm-trusted-firmware/plat/nvidia/tegra/soc/t234/plat_ras.c`

Three functions enhanced with comprehensive seL4 debug output:

1. **`plat_ea_handler()`** - External Abort handler from lower EL
2. **`plat_handle_el3_ea()`** - External Abort while executing in EL3
3. **`tegra234_ras_handler()`** - RAS interrupt handler

**Registers now dumped on every RAS error:**

| Register | Purpose | How it helps debug seL4 |
|----------|---------|------------------------|
| `ELR_EL3` | seL4 PC when error occurred | Which instruction caused the error |
| `SPSR_EL3` | Processor state | Which EL was active (EL0/EL2) |
| `FAR_EL3` | Fault address (EL3) | What address was accessed |
| `ELR_EL2` | seL4 kernel return address | Context within seL4 |
| `ESR_EL2` | Exception syndrome | Type of fault |
| `FAR_EL2` | Fault address (EL2) | What VA caused the fault |
| `SP_EL2` | Stack pointer | seL4 kernel stack state |
| `TTBR0_EL2` | Kernel page table base | Verify kernel PT is valid |
| `VTTBR_EL2` | User page table + VMID | Which user PT was active |
| `VTCR_EL2` | Stage-2 translation control | Verify S2 config is correct |
| `TCR_EL2` | Translation control | Verify TCR matches expected |
| `HCR_EL2` | Hypervisor config | Verify HCR bits are correct |
| `SCTLR_EL2` | System control | MMU/cache enabled? |
| `MAIR_EL2` | Memory attributes | Verify MAIR indices |

**Example output:**
```
============ RAS ERROR CONTEXT (seL4 debug) ============
Exception reason=0 syndrome=0x...
--- EL3 State ---
ELR_EL3 (seL4 PC): 0x...
FAR_EL3: 0x...
SPSR_EL3: 0x... (EL=2)
--- EL2 State (seL4 kernel) ---
ELR_EL2: 0x...
FAR_EL2: 0x...
ESR_EL2: 0x...
SP_EL2:  0x...
--- EL2 MMU Registers ---
TTBR0_EL2 (kernel PT): 0x...
VTTBR_EL2 (user PT):   0x...
VTCR_EL2:  0x...
TCR_EL2:   0x...
HCR_EL2:   0x...
SCTLR_EL2: 0x...
MAIR_EL2:  0x...
=========================================================
```

**Note**: VTTBR_EL2 encodes VMID in bits [63:48] and page table base in bits [47:0]. Decode manually: `VMID = value >> 48`, `base = value & 0xFFFFFFFFFFFF`.

---

### ATF PSCI Register Save/Restore Analysis

**Commit c82013a** added save/restore of EL2 registers across PSCI CPU suspend for pKVM/nVHE:

**Currently saved by ATF (for pKVM):**
| Register | Purpose |
|----------|---------|
| `HCR_EL2` | Hypervisor configuration |
| `CNTHCTL_EL2` | Timer hypervisor control |
| `CNTHP_CTL_EL2` | Hypervisor physical timer control |
| `CNTP_CTL_EL0` | Physical timer control |

**Additional registers seL4 would need if using PSCI CPU suspend:**
| Register | Purpose | Why needed |
|----------|---------|------------|
| `VTTBR_EL2` | User page table + VMID | Stage-2 translation for userspace |
| `VTCR_EL2` | Stage-2 translation control | PA size, start level, granule |
| `TTBR0_EL2` | Kernel page table | Kernel address translation |
| `TCR_EL2` | Translation control | VA size, cacheability |
| `SCTLR_EL2` | System control | MMU enable, caches |
| `MAIR_EL2` | Memory attributes | Cache policy indices |
| `VBAR_EL2` | Exception vector table | Exception handling |
| `TPIDR_EL2` | Thread ID (kernel stack) | seL4 stores kernel stack ptr here |

**Current status**: sel4test does NOT use PSCI CPU suspend, so this is not the cause of current RAS errors. However, if seL4 wants to support CPU power management in the future, ATF would need to be extended to save/restore these registers.

**File**: `plat/nvidia/tegra/soc/t234/plat_psci_handlers.c`

**How to use this output:**
1. `ELR_EL3` → Look up in seL4 symbol table to find faulting function
2. `VTTBR_EL2` → Check if base address is valid (not 0, not garbage)
3. `VTCR_EL2` → Verify SL0 bit matches kernel's UPT_LEVELS
4. `FAR_EL2` → The virtual address that caused the fault
5. Compare all values against expected values in this document

---

## Phase 1: Elfloader Register Audit

**Goal**: Verify what registers elfloader sets before jumping to seL4 kernel.

### Key Elfloader Files

| File | Purpose |
|------|---------|
| `tools/seL4/elfloader-tool/src/arch-arm/armv/armv8-a/64/mmu-hyp.S` | Hypervisor mode MMU setup |
| `tools/seL4/elfloader-tool/src/arch-arm/64/crt0.S` | Entry point |
| `tools/seL4/elfloader-tool/include/arch-arm/64/mode/assembler.h` | Register definitions |

### TCR_EL2 Configuration

**Source**: `mmu-hyp.S:88`

```asm
ldr     x8, =TCR_T0SZ(48) | TCR_IRGN0_WBWC | TCR_ORGN0_WBWC | TCR_SH0_ISH | TCR_TG0_4K | TCR_PS | TCR_EL2_RES1
msr     tcr_el2, x8
```

**Bit breakdown** (for CONFIG_ARM_PA_SIZE_BITS_40):

| Field | Value | Meaning |
|-------|-------|---------|
| T0SZ | 16 | 48-bit virtual address space |
| IRGN0 | 0x1 | Inner write-back, write-allocate |
| ORGN0 | 0x1 | Outer write-back, write-allocate |
| SH0 | 0x3 | Inner shareable |
| TG0 | 0x0 | 4KB granule |
| PS | 0x2 | 40-bit PA (1TB) - from `TCR_PS_1T` |
| RES1 | bits 23,31 | Reserved as 1 |

**Calculated value**: `0x80823510`

**Kernel expectation** (from `machine.h`): `TCR_EL2_DEFAULT = 0x80823510`

**Status**: ✅ **MATCH** - Elfloader and kernel agree on TCR_EL2.

### MAIR_EL2 Configuration

**Source**: `mmu-hyp.S:81-87`

```asm
ldr     x5, =MAIR(0x00, MT_DEVICE_nGnRnE) | \
             MAIR(0x04, MT_DEVICE_nGnRE) | \
             MAIR(0x0c, MT_DEVICE_GRE) | \
             MAIR(0x44, MT_NORMAL_NC) | \
             MAIR(0xff, MT_NORMAL) | \
             MAIR(0xaa, MT_NORMAL_WT)
msr     mair_el2, x5
```

**Index breakdown**:

| Index | Attr Value | Type | Description |
|-------|------------|------|-------------|
| 0 | 0x00 | DEVICE_nGnRnE | Device, non-Gathering, non-Reordering, no Early write ack |
| 1 | 0x04 | DEVICE_nGnRE | Device, non-Gathering, non-Reordering, Early write ack |
| 2 | 0x0c | DEVICE_GRE | Device, Gathering, Reordering, Early write ack |
| 3 | 0x44 | NORMAL_NC | Normal, Inner/Outer non-cacheable |
| 4 | 0xff | NORMAL | Normal, Inner/Outer WB-WA (full caching) |
| 5 | 0xaa | NORMAL_WT | Normal, Inner/Outer Write-through |

**Calculated value**: `0x00aaff440c0400`

**Kernel expectation** (from `vspace.c:40-47`):
```c
enum mair_types {
    DEVICE_nGnRnE = 0,  // → MAIR[0] = 0x00
    DEVICE_nGnRE = 1,   // → MAIR[1] = 0x04
    DEVICE_GRE = 2,     // → MAIR[2] = 0x0c
    NORMAL_NC = 3,      // → MAIR[3] = 0x44
    NORMAL = 4,         // → MAIR[4] = 0xff
    NORMAL_WT = 5       // → MAIR[5] = 0xaa
};
```

**Status**: ✅ **MATCH** - Elfloader MAIR indices match kernel's `enum mair_types`.

### SCTLR_EL2 Configuration

**Source**: `mmu-hyp.S:100` via `enable_mmu` macro in `assembler.h:70-77`

```asm
.macro enable_mmu sctlr tmp
    mrs     \tmp, \sctlr
    orr     \tmp, \tmp, #(1 << 0)   // M bit - MMU enable
    orr     \tmp, \tmp, #(1 << 2)   // C bit - Data cache enable
    orr     \tmp, \tmp, #(1 << 12)  // I bit - Instruction cache enable
    msr     \sctlr, \tmp
    isb
.endm
```

**Bits set by elfloader**:
- Bit 0 (M): MMU enable
- Bit 2 (C): Data cache enable
- Bit 12 (I): Instruction cache enable

**Note**: Other SCTLR_EL2 bits retain their reset values or are set by firmware.

**Observed value**: `0x30c5182d`

### TTBR0_EL2 Configuration

**Source**: `mmu-hyp.S:92-93`

```asm
adrp    x8, _boot_pgd_down
msr     ttbr0_el2, x8
```

Elfloader sets TTBR0_EL2 to its own boot page tables (`_boot_pgd_down`). The kernel later replaces this with `armKSGlobalKernelPGD` during `activate_kernel_vspace()`.

### Stage-2 Memory Attributes (Not MAIR-based)

**Important**: For stage-2 translation (user address spaces via VTTBR_EL2), memory attributes are encoded directly in the page table entry's MemAttr[3:0] field, NOT through MAIR.

From `vspace.c:49-69`:
```c
enum mair_s2_types {
    S2_DEVICE_nGnRnE = 0,
    S2_DEVICE_nGnRE = 1,
    S2_DEVICE_nGRE  = 2,
    S2_DEVICE_GRE = 3,
    S2_NORMAL_INNER_WBC_OUTER_WBC = 15,  // S2_NORMAL
};
```

The kernel uses `S2_NORMAL = 15` for cacheable memory and `S2_DEVICE_nGnRnE = 0` for device memory in stage-2 page tables.

### Elfloader Audit Summary

| Register | Elfloader Sets | Kernel Expects | Status |
|----------|---------------|----------------|--------|
| TCR_EL2 | 0x80823510 | 0x80823510 | ✅ Match |
| MAIR_EL2 | 0x00aaff440c0400 | Indices 0-5 match enum | ✅ Match |
| SCTLR_EL2 | Sets M, C, I bits | Expects these on | ✅ OK |
| TTBR0_EL2 | Boot page tables | Kernel replaces | ✅ OK |

**Conclusion**: Elfloader register setup is correct and matches kernel expectations.

---

## Files Reference

| File | Purpose |
|------|---------|
| `kernel/include/arch/arm/arch/64/mode/machine.h` | TTBR setters, register access |
| `kernel/include/arch/arm/armv/armv8-a/64/armv/vcpu.h` | VTCR, HCR configuration |
| `kernel/include/arch/arm/armv/armv8-a/64/armv/tlb.h` | TLB invalidation |
| `kernel/include/arch/arm/armv/armv8-a/64/armv/context_switch.h` | Context switch |
| `kernel/src/arch/arm/kernel/boot.c` | Boot initialization |
| `kernel/src/arch/arm/64/kernel/vspace.c` | Virtual space management |
| `kernel/src/arch/arm/64/head.S` | Assembly entry, SCTLR setup |
| `tools/seL4/elfloader-tool/src/arch-arm/64/` | Elfloader register setup |

---

## Revision History

| Date | Change |
|------|--------|
| 2025-12-13 | Initial audit document created |
| 2025-12-13 | Added TLB fix documentation |
| 2025-12-13 | Clarified EL0/EL1/EL2 execution model |
| 2025-12-13 | Documented CRITICAL CMake bug fix (AARCH64_VSPACE_S2_START_L1) |
| 2025-12-13 | Added fix verification results from multi-run testing |
| 2025-12-13 | Phase 1: Elfloader register audit - TCR_EL2, MAIR_EL2, SCTLR_EL2 verified |
