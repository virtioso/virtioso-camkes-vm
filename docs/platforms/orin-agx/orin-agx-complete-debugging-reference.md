# Orin AGX Complete Debugging Reference

**MASTER CONSOLIDATED DOCUMENT - Contains all debugging documentation for seL4 on NVIDIA Orin AGX**

This document consolidates all investigation documents, technical references, and debugging guides into a single reference. Use this when investigating issues on Orin AGX.

For the canonical RAS problem statement and current status, see [ras-errors.md](ras-errors.md).

---

# Table of Contents

1. [Current Status and Quick Reference](#1-current-status-and-quick-reference)
2. [RAS Error Investigation Summary](#2-ras-error-investigation-summary)
3. [Memory Layout](#3-memory-layout)
4. [Tegra Cache Operations](#4-tegra-cache-operations)
5. [Firmware Shared Memory](#5-firmware-shared-memory)
6. [Bug B Investigation (FIXED)](#6-bug-b-investigation-fixed)
7. [Diagnostic Region Investigation (ARCHIVED)](#7-diagnostic-region-investigation-archived)
8. [Speculative PTW Analysis](#8-speculative-ptw-analysis)
9. [Safe PTE Multi-Level Audit](#9-safe-pte-multi-level-audit)
10. [Memzero Page Table Audit](#10-memzero-page-table-audit)
11. [ARM64 Whole-Cache Operations](#11-arm64-whole-cache-operations)
12. [ARM64 Cache Maintenance and Barriers](#12-arm64-cache-maintenance-and-barriers)
13. [ARM64 MM Register Audit](#13-arm64-mm-register-audit)
14. [SDEI RAS Error Handling](#14-sdei-ras-error-handling)
15. [UART Binary Transfer for Debugging](#15-uart-binary-transfer-for-debugging)
16. [Upstream addrFromKPPtr Bugs](#16-upstream-addrfromkpptr-bugs)
17. [Build and Test Procedures](#17-build-and-test-procedures)

---

# 1. Current Status and Quick Reference

## Status (2025-12-20)

| Bug | Error Pattern | Status | Root Cause |
|-----|---------------|--------|------------|
| **A** | 0x7fffxxxx | ⚠️ IN PROGRESS | PTE overwrite mystery - stale data during VSpace switches |
| **B** | 0x0fc0/0x0ff0 | ✅ FIXED | Zero PTEs during unmap - fixed with safe PTE pattern |

## Key Findings

1. **Cache operations work correctly** - dc civac flushes data to DRAM
2. **ATF/OP-TEE not corrupting memory** - 5-second delay test proves this
3. **Firmware memory regions don't conflict** - TZDRAM at 0x50000000, not 0x80000000
4. **Corruption happens during test execution** - not at boot or idle
5. **Only 7 of 122 tests trigger errors** - specific code paths involved
6. **Primary triggers**: FPU0001 (76%), CANCEL_BADGED_SENDS_0002 (32%)

## Critical Rules

| Rule | Description |
|------|-------------|
| **Use dc civac, never dc cisw** | dc cisw broken on ALL Tegra platforms |
| **RAM start at 0x80032000** | Avoids RAS errors from speculative PTW |
| **Use safe PTEs** | pte_pte_invalid_new() must have bits[47:12] pointing to valid DRAM |
| **Always use MCP tools** | mcp__sel4-autopilot__build_sel4test for clean builds |

## What NOT to Investigate (Proven Working/Ruled Out)

- Cache flush operations (dc civac works correctly)
- Arch_createObject() initialization (verified correct)
- ATF/OP-TEE memory corruption (disproven)
- Firmware memory conflicts (TZDRAM at 0x50000000)
- ATF SMC handlers accessing NS DRAM (they don't)
- Basic IPC, scheduling, CNode ops (never trigger errors)

## Build Configurations

```bash
# Standard hypervisor mode
make orinagx_defconfig && make sel4test

# With function tracing (ftrace)
make orinagx_ftrace_defconfig && make sel4test
```

---

# 2. RAS Error Investigation Summary

## Two Separate Bugs

Analysis across different RAM configurations revealed **TWO COMPLETELY INDEPENDENT BUGS**:

| Bug | Error Pattern | Trigger Test | Code Path | RAM Dependent | Status |
|-----|---------------|--------------|-----------|---------------|--------|
| **A** | 0x7fffxxxx | FPU0001 (>99%) | `arm_sys_send_recv`, `load_segment` | YES - only near 0x80000000 | ⚠️ PTE overwrite mystery |
| **B** | 0x0fc0/0x0ff0 | All tests (CANCEL_BADGED most) | `sel4utils_destroy_process`, `vka_cnode_revoke` | NO - always present | ✅ FIXED |

## RAM Address Bisection Results

Binary search found exact RAM start address boundary where Bug A occurs:

| RAM Start | Offset from 0x80000000 | Status | RAS Errors |
|-----------|------------------------|--------|------------|
| 0x80000000 | +0 KB | ❌ ERRORS | ~870 |
| 0x80020000 | +128 KB | ❌ ERRORS | ~100+ |
| **0x80031000** | **+196 KB** | **❌ ERRORS** | **2** |
| **0x80032000** | **+200 KB** | **✅ WORKS** | **0** |
| 0x80040000 | +256 KB | ✅ WORKS | 0 |
| 0x90000000 | +256 MB | ✅ WORKS | 0 |

**BOUNDARY: 0x80032000 (200KB offset from DRAM base)**

## Investigation Phases Summary

### Phase 16: Non-Temporal Stores (STNP)
**Result**: STNP that bypass cache entirely did NOT fix the problem.
**Conclusion**: Cache flush is NOT the problem - DRAM receives correct values.

### Phase 17: Delayed Verification Test
**Result**: 5-second delay between PT creation and verification - verification PASSES, but later scan finds garbage.
**Conclusion**: ATF/OP-TEE hypothesis RULED OUT. Corruption happens DURING test execution.

### Phase 18: Function Tracing (ftrace)
**Result**: 65,536 kernel function calls captured - NO PT modification functions called.
**Conclusion**: No explicit PT modification occurs, yet garbage appears. Points to memory aliasing or hardware issue.

### Phase 19: 200KB Diagnostic Region
**Result**: Diagnostic pattern at 0x80000000-0x80032000 remained INTACT. PT corruption occurs at 0xACxxxxxx (middle of DRAM).
**Conclusion**: First 200KB of DRAM is NOT being corrupted. RAM start address affects RAS manifestation, not the underlying corruption.

### Phase 20: Functional Correctness Reframe
**Key Insight**: sel4test passes 100% despite RAS errors. The system is functionally correct.
**Conclusion**: RAS errors come from **speculative** page table walks that don't affect architectural execution.

## The Real Problem

RAS errors come from speculative page table walks that don't affect architectural execution.

The "garbage" in PTEs is stale data from previous test iterations:
1. Previous test iteration created PT at some address
2. Test finished, PT memory was freed
3. Memory reused for user data
4. Some stale L2 entry still points to old address
5. **Speculative walker** follows the stale link → sees garbage → RAS error

But the test works because the **architecturally active** VSpace doesn't use that stale path.

## Thread Count Threshold

| Threads | Error Rate |
|---------|------------|
| 1-3 (concurrent) | **0%** |
| **4+ (concurrent)** | **24-32%** |

The bug requires **exactly 4+ concurrent threads** at same priority.

---

# 3. Memory Layout

## Normal Mode Memory Layout (RAM at 0x80000000)

When using `orinagx_defconfig` or `orinagx_ftrace_defconfig`:

```
Physical Address    Section      Size    Contents
─────────────────────────────────────────────────────────────
0x80000000         .boot        64KB    Kernel boot code
  0x80000000       _start               Entry point
  0x80000070       init_pt_with_safe_ptes
  0x80003750       init_kernel

0x80010000         .boot.bss    ~8KB    Boot data structures
  0x80010000       rootserver
  0x800100b8       ndks_boot

0x80012000         .text        ...     Main kernel code
  0x80012000       ki_boot_end          End of boot section
  0x80020000       arm_vector_table     Exception vectors (LIVE!)
  0x80020784       invalid_vector_entry
  0x8002080c       lower_el_sync
  ...

0x80048000         .bss         2.1MB   Kernel BSS (page tables, etc.)
```

### Post-Boot Memory Access

| Range | Section | Post-boot access |
|-------|---------|------------------|
| 0x80000000 - 0x80010000 | .boot | **Never** - boot code only |
| 0x80010000 - 0x80012000 | .boot.bss | **Never** - boot data |
| 0x80012000 - 0x8001ffff | .text (start) | **Never** - empty |
| 0x80020000+ | .text (live) | **Active** - runtime kernel code |

**Safe to repurpose after boot**: 0x80000000 - 0x8001ffff (128KB)

**Critical: 0x80020000 Contains Live Code** including `arm_vector_table` used for EVERY interrupt/exception.

## EFI Runtime Structures

Located at ~32GB, well above seL4's region:

| Structure | Address | Location |
|-----------|---------|----------|
| RNG | 0x815120018 | ~32.3 GB |
| MEMRESERVE | 0x816054f98 | ~32.4 GB |
| SMBIOS 3.0 | 0x827960000 | ~32.6 GB |

**No conflict**: seL4 uses 2-2.75GB, EFI structures at ~32GB.

## Address Translation

```
Kernel Virtual Address = Physical Address + KERNEL_OFFSET
                       = Physical Address + 0x8000000000

Example:
  _start virtual:  0x8080000000
  _start physical: 0x0080000000
  KERNEL_OFFSET:   0x8000000000
```

---

# 4. Tegra Cache Operations

## Summary

On NVIDIA Tegra Xavier and Orin SoCs, the ARM `dc cisw` (clean and invalidate by set/way) instruction does not work correctly. **Use `dc civac` (clean and invalidate by virtual address to Point of Coherency) instead.**

## Problem

The `dc cisw` instruction **invalidates cache lines without properly writing data to DRAM first**, resulting in data loss.

### Symptoms
- Page table entries appear corrupted after cache flush
- Memory contents read back as garbage/uninitialized values after flush
- Random crashes after MMU enable with correct page table setup

### Root Cause

The `dc cisw` instruction operates on the local CPU's view of the cache using set/way addressing. On complex SoCs like Tegra with multiple cache levels, system-level caches, and cache coherency interconnects, the set/way operation may not properly propagate to all levels.

## Solution

Replace `dc cisw` with `dc civac`:

```asm
/* Before (broken on Tegra) */
dc      cisw, x11      /* clean and invalidate by set/way */

/* After (works on Tegra) */
dc      civac, x0      /* clean and invalidate by VA to PoC */
```

## Affected Components

| Component | File | Status |
|-----------|------|--------|
| elfloader | `src/arch-arm/armv/armv8-a/64/mmu.S` | ✅ FIXED |
| seL4 kernel generic | `kernel/src/arch/arm/armv/armv8-a/64/cache.c` | ✅ Conditional |
| seL4 kernel Orin | `kernel/src/plat/orinagx/machine/cache.c` | ✅ Platform-specific |

## Platform Applicability

| Platform | dc cisw | dc civac |
|----------|---------|----------|
| Tegra Xavier (T194) | BROKEN | Works |
| Tegra Orin (T234) | BROKEN | Works |
| Raspberry Pi 4 (BCM2711) | Works | Works |
| QEMU virt | Works | Works |

**Always prefer `dc civac` for maximum portability on ARM platforms.**

---

# 5. Firmware Shared Memory

## Summary

| Component | Uses Shared Memory? | Location | Conflicts with seL4? |
|-----------|---------------------|----------|----------------------|
| **UEFI** | YES | NS DRAM (runtime) at ~32GB | **NO** |
| **ATF** | YES | SYSRAM (0x40000000 range) | **NO** |
| **OP-TEE** | YES | Dynamic registration only | **NO** |

## Critical Finding: TZDRAM Location

Despite OP-TEE source showing `CFG_TZDRAM_START = 0x80000000`, the **actual TZDRAM is at 0x50000000**, not 0x80000000.

**ATF platform configuration for T234 (Orin AGX):**

```makefile
PLAT_BL31_BASE := 0x50000000
TZDRAM_SIZE := 0x00400000  // 4MB
```

**Actual TZDRAM region: 0x50000000 - 0x50400000 (4MB)**

This is in the SYSRAM/secure region below 0x80000000, NOT in main DRAM.

## Memory Layout Summary

| Region | Address Range | Size | Owner |
|--------|--------------|------|-------|
| MMIO / Peripherals | 0x00000000 - 0x4FFFFFFF | ~1.25GB | Hardware |
| **TZDRAM (ATF+OP-TEE)** | **0x50000000 - 0x50400000** | **4MB** | **Secure World** |
| System RAM (NS) | 0x80000000 - 0xFFFFFFFF | ~2GB | Normal World |
| System RAM (NS) | 0x100000000 - ~0x900000000 | ~30GB+ | Normal World |

**Conclusion:** seL4's load address at 0x80000000 does NOT conflict with OP-TEE's TZDRAM at 0x50000000.

## ATF SMC Handling

ATF runs at EL3 when handling SMC calls. Investigation shows it does NOT access NS DRAM at 0x80000000+ during normal operation:

- PSCI handlers only interact with MCE (Microcontroller Engine) via ARI
- Cache flush operations during PSCI are on TZDRAM only (secure memory at 0x50000000)
- RAS handlers read error status from GIC600AE registers (MMIO), not DRAM

---

# 6. Bug B Investigation (FIXED)

## Summary

**Bug B** produced RAS errors with addresses 0x0fc0/0x0ff0 (near-NULL) during capability revocation and process destruction.

| Property | Value |
|----------|-------|
| Error addresses | 0x0fc0, 0x0ff0 (cache-line aligned, near NULL) |
| Primary trigger | CANCEL_BADGED_SENDS_0002 test |
| Code path | `sel4utils_destroy_process`, `vka_cnode_revoke` |
| RAM dependency | None - occurred at any RAM base address |

## Root Cause

`pte_pte_invalid_new()` returns a 64-bit value with **ALL BITS ZERO**.

When speculative PTW reads the zero PTE, bits[12-47] (page_base_address) = 0. The walker tries to access physical address 0x0000 → RAS error.

## Race Window

```c
unmapPage():
  1203:  *(lu_ret.ptSlot) = pte_pte_invalid_new();  // PTE = 0   ← RACE START
  1205:  cleanInvalByVA(...);                        // dc civac
  1209:  invalidateTLBByASID(asid);                  // TLBI      ← RACE END
```

Between lines 1203 and 1209, Stage 2 is ENABLED and another core can speculatively walk and read the ZERO PTE.

## Fix

Added `pte_safe_invalid_new()` which sets bits[47:12] to `armKSGlobalUserVSpace` PA while keeping bits[1:0]=0 (invalid):

```c
static inline pte_t pte_safe_invalid_new(void)
{
    pte_t pte;
    paddr_t safe_pa = addrFromKPPtr(armKSGlobalUserVSpace);
    pte.words[0] = safe_pa & 0xfffffffff000ull;
    return pte;
}
```

**Why this works:**
- The PTE is still "invalid" (bit[0]=0, bit[1]=0)
- Hardware won't use it for actual translation
- But speculative PTW sees a safe PA in bits[47:12]
- Instead of accessing PA=0 (causing RAS error), it speculatively accesses armKSGlobalUserVSpace which is valid kernel memory

## Verification Results

| Test Run | 0x0fc0/0x0ff0 Errors (Bug B) | 0x7fffxxxx Errors (Bug A) |
|----------|------------------------------|---------------------------|
| Before fix | 56-92 per run | Present |
| **After fix** | **0 per run** | Present |

**BUG B IS FIXED!**

---

# 7. Diagnostic Region Investigation (ARCHIVED)

> **Note**: The diagnostic region feature has been removed from the codebase.
> This section is preserved for historical reference only.

## Background

Previous investigations showed moving RAM start from 0x80000000 to 0x80032000 (200KB offset) eliminated RAS errors. This tested whether something specifically corrupts the first 200KB of DRAM.

## Results

| Metric | Value |
|--------|-------|
| Tests run | 7 (CANCEL_BADGED_SENDS_0002 x7) |
| Tests passed | 7 (100%) |
| RAS errors | **0** |
| Diagnostic region corruptions | **0** |
| PT corruptions detected | **4** |

PT Corruption Events (all in middle of DRAM, not at beginning):
- PT@0xac25c000, PT@0xac234000, PT@0xac1e6000, PT@0xac254000

## Conclusion: Hypothesis REJECTED

The first 200KB of DRAM is **NOT being corrupted**:
- Diagnostic pattern remained intact through 72+ million function calls
- PT corruption occurs in middle of DRAM (0xACxxxxxx range), not at beginning
- Moving RAM start to 0x80032000 prevents RAS errors for a different reason

**Feature removed**: The diagnostic region feature was removed since it was not useful for debugging the actual issue

---

# 8. Speculative PTW Analysis

## Critical Correction (2025-12-17)

Testing with ARM_HYPERVISOR_SUPPORT=OFF revealed that **speculative page table walks (PTW) are NOT EL2-specific** - they occur at ALL exception levels on modern AArch64 SoCs.

| Configuration | Safe PTEs | RAS Errors per Run |
|---------------|-----------|-------------------|
| ARM_HYP=OFF | No | **556** |
| ARM_HYP=OFF | Yes | **0** |
| ARM_HYP=ON | Yes | **0-4** |

The ~200x higher error rate in EL1-only mode proved that EL2 mode was *masking* the bug, not causing it.

## Architectural Understanding

**Corrected understanding**: Speculative PTW is a **universal ARM64 phenomenon**:
- Occurs at EL1 (non-hypervisor kernels)
- Occurs at EL2 (hypervisor mode)
- Any zero PTE can trigger hardware accesses to PA=0

## Code Paths Audited

The safe PTE fix was applied to:
- ✅ `unmapPage()` - page unmap
- ✅ `unmapPageTable()` - page table unmap
- ✅ VSpaceObject creation
- ✅ PageTableObject creation
- ✅ `armKSGlobalUserVSpace` initialization

---

# 9. Safe PTE Multi-Level Audit

## Executive Summary

The current `pte_pte_invalid_new()` implementation IS architecturally correct for ALL page table levels (L0-L3), not just L3.

## Current Safe PTE Format

```c
#define pte_pte_invalid_new() \
    ((pte_t){ .words[0] = addrFromPPtr(armKSGlobalUserVSpace) & 0xfffffffff000ull })
```

This creates an entry with:
- **bits[1:0] = 0b00**: Invalid descriptor (per ARM spec, valid at ANY level)
- **bits[47:12]**: Physical address of `armKSGlobalUserVSpace` (valid DRAM)

## ARM64 Stage-2 Descriptor Types

| bits[1:0] | L0/L1/L2 Meaning | L3 Meaning |
|-----------|------------------|------------|
| 0b00 | Invalid | Invalid |
| 0b01 | Block (1GB/2MB) | Reserved (Invalid) |
| 0b10 | Invalid | Invalid |
| 0b11 | Table | Page (4KB) |

**Key insight**: `0b00` is ALWAYS invalid, regardless of translation level.

## Audit Results

All creation and deletion sites have been verified to use safe PTEs:
- VSpace create/delete
- PageTable create/delete
- unmapPage/unmapPageTable
- performPageTableInvocationUnmap
- Boot-time initialization

**Conclusion: No changes required.** The current implementation is architecturally correct.

---

# 10. Memzero Page Table Audit

## Executive Summary

**ARM64 is safe.** All page table initialization paths use `pte_pte_invalid_new()` with safe addresses. The `memzero()` calls occur BEFORE safe PTE initialization, and memory isn't visible to MMU until after safe PTEs are written and flushed.

## ARM64 (Orin AGX target) - ✓ SAFE

| Location | Function | What it does | Safe? |
|----------|----------|--------------|-------|
| objecttype.c:561 | `Arch_createObject()` | `memzero(pt)` followed by safe PTE writes + flush | ✓ Yes |
| vspace.c:302-304 | `map_kernel_window()` | Safe PTE writes | ✓ Yes |
| vspace.c:406-416 | `init_pt_with_safe_ptes()` | Writes safe PTEs + cache flush | ✓ Yes |

**Key observation**: ARM64 never uses `clearMemory()` or `clearMemory_PT()` for page tables. All paths use explicit safe PTE writes.

## Race Condition Analysis

**Q: Can page tables be visible to MMU before safe PTEs are written?**

**No.** The sequence is:
1. `resetUntypedCap()` → `clearMemory()` zeros memory (preemption possible, but memory is just "untyped")
2. `createNewObjects()` → `Arch_createObject()` writes safe PTEs, cache flush (NO preemption)
3. Cap returned → user has page table cap (table not mapped yet)
4. User maps table → parent entry updated (NOW table is visible to MMU - but safe PTEs already in place)

---

# 11. ARM64 Whole-Cache Operations

## Summary

Set/way cache maintenance operations (dc cisw, dc csw) are architecturally broken on modern ARM64 SoCs, not just NVIDIA Tegra. Linux ARM64 removed `flush_cache_all()` entirely in 2015.

## Why Set/Way Operations Are Broken (ALL ARM64)

1. **Race with CPU speculation** - Background behavior may allocate/evict/migrate cache lines
2. **Not broadcast** - Set/way operations don't affect other CPUs
3. **Don't affect system caches** - System caches (L3, LLC) only respect VA-based maintenance
4. **Only work with caches disabled** - These operations don't make sense while SCTLR.C or SCTLR.M are set

## seL4 Whole-Cache Functions Affected

| Function | Purpose | Problem |
|----------|---------|---------|
| `clean_D_PoU()` | Clean D-cache to PoU | Uses dc csw (set/way) |
| `cleanInvalidate_D_PoC()` | Clean+invalidate D-cache to PoC | Uses dc cisw (set/way) |
| `cleanInvalidate_L1D()` | Clean+invalidate L1 D-cache | Uses dc cisw (set/way) |

## Solution: VA-Based Range Operations

Instead of "flush entire cache", flush specific memory regions by virtual address:

```c
// Instead of: cleanInvalidate_D_PoC()
// Use: cleanInvalidateCacheRange_RAM(start, end, pstart)
```

---

# 12. ARM64 Cache Maintenance and Barriers

## The Three "Locations"

### Point of Unification (PoU)
Where instruction side and data side agree. `dc cvau` exists mainly for **I-cache coherence on the local core**.

### Point of Coherency (PoC)
Where **all observers that participate in coherency** can agree on a single value.
- `dc cvac` cleans a line **to PoC** (write back, keep valid)
- `dc civac` cleans **and invalidates** to PoC

**SMP consequence:** Cleaning to PoC ensures the memory system has updated bytes, but doesn't magically update other cores' caches.

## Barriers

### DMB — Data Memory Barrier
Orders **memory accesses** before/after it. Does NOT guarantee completion of cache maintenance.

### DSB — Data Synchronization Barrier
Orders and **waits until completion** of memory accesses AND maintenance operations. **DSB is the "make it real" instruction** for cache maintenance.

### ISB — Instruction Synchronization Barrier
Flushes pipeline so subsequent instructions use new architectural state.

## Barrier Scope Suffixes

| Suffix | Domain | Use Case |
|--------|--------|----------|
| `ish` | Inner Shareable | CPU↔CPU shared RAM |
| `osh` | Outer Shareable | CPU↔device (DMA) |
| `nsh` | Non-shareable | Local-ish, rarely correct for SMP |
| `sy` | Full system | Strongest, usually slowest |

## Canonical Recipes

### Flush D-cache lines to PoC (clean + invalidate)
```asm
dsb ishst
dc  civac, Xt  // per line
dsb ish
```

### Cross-core code execution
**Producer:**
```asm
dsb ishst
dc  cvac, Xt   // per line (to PoC, not PoU!)
dsb ish
// signal other core(s)
```

**Receiver:**
```asm
ic  ivau, Xt   // per line
dsb ish
isb
```

## Common Pitfalls

1. **Wrong shareability in page tables** - If mapped Non-shareable, no amount of `ish` barriers will work
2. **Invalidate when dirty** - `dc ivac` can silently drop dirty data. Use `civac` unless you can prove safety.
3. **Missing the "after" DSB** - Cache ops can be pending; without dsb, your code may race
4. **PoU used for cross-core visibility** - Works "sometimes", fails "mysteriously"

---

# 13. ARM64 MM Register Audit

## Exception Level Architecture

In seL4 with `CONFIG_ARM_HYPERVISOR_SUPPORT` enabled:

| Exception Level | What Runs Here | Translation Mechanism |
|-----------------|----------------|----------------------|
| **EL0** | Native userspace threads | Stage-2 via VTTBR_EL2 |
| **EL1** | Guest OS (vCPU only) | Stage-1 + Stage-2 |
| **EL2** | seL4 kernel | Stage-1 via TTBR0_EL2 |

**Critical insight**: For sel4test (native seL4 threads, NOT VMs), EL1 registers (TCR_EL1, TTBR0_EL1, MAIR_EL1) being zero is **expected and correct**.

## Key EL2 Registers

### TTBR0_EL2
- **Purpose**: Stage-1 translation for kernel (EL2) memory accesses
- **Set by**: `setCurrentKernelVSpaceRoot()`
- **Value at boot**: Points to `armKSGlobalKernelPGD`

### VTTBR_EL2
- **Purpose**: Stage-2 translation for EL0/EL1 memory accesses
- **Format**: `[63:48] = VMID, [47:0] = page table base address`
- **Set by**: `setCurrentUserVSpaceRoot()`

### TCR_EL2
- **Purpose**: Controls stage-1 translation for EL2
- **Expected value**: `0x80823510` (TCR_EL2_DEFAULT)
- **Set by**: Elfloader (NOT kernel!)

### VTCR_EL2
- **Purpose**: Controls stage-2 translation (for EL0/EL1 accesses)
- **Set by**: `vcpu_init_vtcr()` during kernel boot

### HCR_EL2
- **Purpose**: Controls hypervisor behavior, enables stage-2 translation
- Key bits:
  - `HCR_VM` (bit 0): Virtualization MMU enable - enables stage-2 translation
  - `HCR_TGE` (bit 27): Trap general exceptions - routes EL0 exceptions to EL2
  - `HCR_DC` (bit 12): Default cacheable - disables stage-1 translation at EL1

## Address Translation Flow

### Native Thread (EL0) Memory Access
```
EL0 Virtual Address
        ↓
   [Stage-2 Only - HCR_EL2.DC=1 disables Stage-1]
        ↓
   VTTBR_EL2 (page table base + VMID)
        ↓
   VTCR_EL2 (translation control)
        ↓
Physical Address
```

### Kernel (EL2) Memory Access
```
EL2 Virtual Address
        ↓
   [Stage-1 Only]
        ↓
   TTBR0_EL2 (page table base)
        ↓
   TCR_EL2 (translation control)
        ↓
Physical Address
```

## Critical Bug Found and Fixed

**CMake configuration bug** in `kernel/src/arch/arm/config.cmake`:

```cmake
# WRONG - ARM_HYPERVISOR_SUPPORT is C define name, not CMake variable
if(KernelArmPASizeBits40 AND ARM_HYPERVISOR_SUPPORT)

# CORRECT - KernelArmHypervisorSupport is the CMake variable
if(KernelArmPASizeBits40 AND KernelArmHypervisorSupport)
```

**Impact**: This caused hardware/software mismatch in page table levels, leading to translation and address size faults.

---

# 14. SDEI RAS Error Handling

## Overview

When a RAS error occurs on Tegra Orin (T234):

```
Hardware RAS Error
    ↓
ATF RAS Handler (EL3)
    ↓
SDEI Event Dispatch (fails - no handler)
    ↓
ATF logs error to console
    ↓
Done (no OS notification)
```

## ARM RAS ADDR Register Encoding

**CRITICAL**: The ADDR field in RAS error records is NOT a raw physical address!

| Bits | Field | Description |
|------|-------|-------------|
| **63** | **NS** | Non-secure attribute (1 = non-secure access) |
| 62 | SI | Secure Incorrect (1 = NS field may be wrong) |
| 61 | AI | Address Incorrect (1 = PADDR may be wrong) |
| 60 | VA | Virtual Address (1 = address is VA, not PA) |
| **55:0** | **PADDR** | **Actual physical address** |

**Example**: `ADDR = 0x8000000000000000`
- Bit 63 (NS) = 1 → Non-secure access
- Bits 55:0 = 0x0 → **Physical address is ZERO (NULL pointer access!)**

This is NOT "bit 63 set = invalid address". It means **PA=0x0 accessed from non-secure world**.

## Critical Finding: Tegra Has No HEST Table

**Tegra Orin does NOT provide an ACPI HEST (Hardware Error Source Table).**

Without HEST:
1. No GHES driver probe
2. No SDEI-GHES integration
3. No error status blocks
4. No memory failure handling

**What actually happens:**
```
ATF RAS Error Detected
    ↓
ATF logs error to console
    ↓
ATF calls sdei_dispatch_event(300 + cpu_id)
    ↓
sdei_dispatch_event returns -1 (no handler)
    ↓
Error handling ends (no OS notification)
```

## Implications for seL4

1. **No standard error delivery path** - Even if seL4 implemented SDEI client, there's no GHES infrastructure
2. **ATF-only error handling** - Tegra relies on ATF to handle RAS errors completely
3. **CBB errors separate** - Tegra CBB fabric errors use custom interrupt-based handler

---

# 15. UART Binary Transfer for Debugging

## Recommended: COBS (Consistent Overhead Byte Stuffing)

For seL4 / kernel-level debug on platforms like Jetson AGX Orin:

**COBS-framed binary chunks with CRC32**

This gives:
- Minimal overhead (~0.4% worst case)
- Fast resynchronization
- Clean host-side tooling
- Small, removable code footprint

## Comparison

| Method | Overhead | Resync | Complexity |
|--------|---------|--------|------------|
| Base64 | +33% | Yes | Medium |
| Hex | +100% | Yes | Low |
| SLIP | Low | Good | Very low |
| HDLC/PPP | Medium | Excellent | Medium |
| **COBS** | **Very low** | **Excellent** | **Low** |

## Chunking Strategy

- Payload size: **512–2048 bytes**
- Each frame independently checksummed (CRC32)
- Receiver writes chunks by `(blob_id, seq)`

---

# 16. Upstream addrFromKPPtr Bugs

## Summary

The seL4 kernel contains several uses of `addrFromKPPtr()` where `addrFromPPtr()` should be used. These are upstream bugs present in seL4/master.

## Background

- `addrFromKPPtr(ptr)`: Converts kernel ELF mapping pointer to physical address. Only valid for addresses in `[KERNEL_ELF_BASE, KERNEL_ELF_TOP]`.
- `addrFromPPtr(ptr)`: Converts physical memory window pointer to physical address. Valid for all PPTR addresses including `.bss` section.

The global kernel page table structures are in `.bss` (PPTR region), NOT kernel ELF region. Therefore `addrFromPPtr()` is correct.

## Why It Works Anyway

On most platforms, `KERNEL_ELF_BASE_OFFSET == PPTR_BASE_OFFSET`, so both functions produce the same result.

## Affected Locations in vspace.c

1. `map_kernel_window()` - 4 bugs (armKSGlobalKernelPUD, armKSGlobalKernelPDs, armKSGlobalKernelPT)
2. `activate_kernel_vspace()` - 2 bugs (armKSGlobalKernelPGD, armKSGlobalUserVSpace)
3. `setVMRoot()` - 2 bugs (armKSGlobalUserVSpace fallback)
4. `performPageTableInvocationMap()` - 1 bug (armKSGlobalLogPTE)

**Total: 9 bugs in vspace.c**

---

# 17. Build and Test Procedures

## MCP Tools (Recommended)

```python
# Build
mcp__sel4-autopilot__build_sel4test(mode="el2")  # or "el1", "el2-ftrace"

# Test single run
mcp__sel4-autopilot__test_sel4_binary(binary_path="...")

# Stress test (multiple runs)
mcp__sel4-autopilot__test_sel4_multi_run(binary_path="...", run_count=10)

# Get logs
mcp__sel4-autopilot__get_sel4_log(request_id="...")
```

## Analyzing Results

```bash
# Analyze sel4test log for RAS errors
/home/hlyytine/pkvm/autopilot/analyze_sel4log.py <results>/sel4.log

# Decode ftrace binary data
/home/hlyytine/tii-sel4/kernel/tools/decode_ftrace_binary.py <ftrace_data>
```

## Key Code Locations

| File | Purpose |
|------|---------|
| `kernel/src/arch/arm/64/kernel/vspace.c` | Page table operations, safe PTE init |
| `kernel/src/object/untyped.c` | Untyped retype, memory clearing |
| `kernel/src/benchmark/ftrace.c` | Function tracing |
| `kernel/src/plat/orinagx/machine/cache.c` | Tegra-specific cache ops (dc civac) |
| `kernel/tools/dts/orinagx.dts` | Device tree |

## Results Location

- Filtered seL4 output: `/home/hlyytine/pkvm/autopilot/results/<timestamp>/sel4.log`
- Raw UART capture: `/home/hlyytine/pkvm/autopilot/results/<timestamp>/uart-raw.log`
- Multi-run logs: `/home/hlyytine/pkvm/autopilot/results/<timestamp>/run_N/sel4.log`

---

# Document History

| Date | Change |
|------|--------|
| 2025-12-20 | Consolidated document created from all debugging references |

---

*End of consolidated debugging reference*
