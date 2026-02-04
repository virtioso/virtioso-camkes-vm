# ARM64 Cache Operations Refactoring Plan

**Date**: 2025-12-25
**Status**: Approved for implementation

## Summary

Move seL4 ARM64 **kernel** from broken set/way cache operations (`dc cisw`) to VA-based operations (`dc civac`) as the default for all ARM64 platforms.

**Why**: Set/way operations are architecturally broken on ALL modern ARM64 SoCs (not just Tegra). Linux ARM64 removed `flush_cache_all()` in 2015 for this reason.

> "The documented semantics of flush_cache_all are not possible to provide for arm64"
> — Mark Rutland, ARM Ltd

---

## Deliverables

### 1. Documentation: `docs/reference/arm64-cache-barrier-audit.md`

A comprehensive document justifying ALL kernel and elfloader cache/barrier changes:
- Each change with ARM architecture justification
- Linux/KVM/Xen precedent references
- Discussion of PoU vs PoC requirements
- Barrier usage patterns

### 2. Code: Single commit on top of `8b543b5d1`

Implementing VA-based cache ops for all ARM64 platforms.

---

## Git Workflow

**Base commit**: `8b543b5d1` (FOR-UPSTREAM: arm64: Use valid PT base during VMID switch)

**Steps:**
1. Checkout `8b543b5d1`
2. Implement the changes described below (single commit)
3. Rebase the subsequent commits on top

**Commits to rebase** (30 commits from 8b543b5d1..HEAD):
- Some will apply cleanly (ftrace, docs, etc.)
- Some will conflict and need resolution (cache-related commits)
- Some may become obsolete (platform-specific cache.c additions)

**Expected conflicts to resolve:**
| Commit | Likely Conflict | Resolution |
|--------|-----------------|------------|
| `0d9b3bad9` arm: Disable dc cisw for Tegra | Adds Orin cache.c | Skip - our commit supersedes |
| `f85e5810e` arm64: Improve Tegra cache ops | Modifies same files | Merge changes carefully |
| `5b36dbb8a` arm: Fix PT cache coherency | PoC changes | Keep PoC approach, resolve conflicts |
| `802c14d46` arm64: Fix VSpace/PT creation | PoC changes | Keep PoC approach |

## Scope Analysis

| Component | Current State | Action Needed |
|-----------|---------------|---------------|
| **Kernel ARM64 whole-cache** | Set/way (`dc cisw`) | **FIX** - replace with VA-based |
| **Kernel ARM64 range ops** | Already VA-based | **No change** (already correct) |
| **Kernel ARM32** | Set/way (coprocessor) | No change |
| **Elfloader ARM64** | Already VA-based (`dc civac`) | **No change needed** |
| **Elfloader ARM32** | Set/way (broken on Tegra) | Out of scope (ARM32) |

**Good news**: The elfloader's `flush_dcache_range()` in `mmu.S` already uses `dc civac`!

## PoU vs PoC Analysis

**The distinction is valid and MUST be preserved:**

| Level | Instruction | Purpose | Used For |
|-------|-------------|---------|----------|
| **PoU** | `dc cvau` | I-cache/D-cache coherency | Instruction unification, JIT |
| **PoC** | `dc cvac`/`dc civac` | System-wide coherency | **MMU page table walker**, DMA |

**Why this matters:**
> "Page tables are read by the MMU hardware walker which operates at Point of Coherency (PoC).
> We must use dc civac instead of dc cvau to ensure zeros are visible to the MMU walker."
> — seL4 kernel comment in arch/machine.h

**seL4's range-based operations are already correct:**
- `cleanCacheRange_PoU()` → `dc cvau` (PoU) - for instruction unification ✓
- `cleanInvalidateCacheRange_RAM()` → `dc civac` (PoC) - for page tables ✓

**The ONLY problem is whole-cache operations:**
- `clean_D_PoU()` and `cleanInvalidate_D_PoC()` use broken set/way operations
- NOT a PoU vs PoC issue - BOTH are broken because they use `dc cisw`/`dc csw`

**SMP considerations (already handled):**
- I-cache: `ic ialluis` (broadcast) in SMP, `ic iallu` (local) in UP
- D-cache: Same ops, but `cleanInvalidateCacheRange_RAM()` has extra clean for MP safety

**From project documentation (`arm64-cache-maintenance-barriers-smp.md`):**
> "Prefer **PoC** for any maintenance that affects shared kernel objects, page tables, IPC buffers,
> or anything another core might touch... This is not the fastest approach. It *is* the one that
> tends to produce a working kernel on complicated SoCs."

**Strategy**: Keep using PoC for shared structures (page tables, IPC). PoU only for same-core I/D coherency.

## Caution Note

The Orin AGX RAS issues have not been fully root-caused. The changes to VA-based ops and PoC
may be:
- Correct fixes for real architectural issues, OR
- Workarounds that mask deeper problems

**Recommendation**: These changes are architecturally sound per ARM documentation (Linux removed
set/way ops in 2015), so they should be safe to apply broadly. But monitor for issues on other
platforms.

## Files to Modify

| File | Action |
|------|--------|
| `src/arch/arm/armv/armv8-a/64/cache.c` | Replace set/way with VA-based ops |
| `src/plat/orinagx/config.cmake` | Remove platform-specific cache override |
| `src/plat/orinagx/machine/cache.c` | Delete (no longer needed) |

---

## Detailed Change List with Justifications

### Change 1: Replace `dc cisw` (set/way) with `dc civac` (VA-based)

**Location**: `src/arch/arm/armv/armv8-a/64/cache.c`

**What changes**:
- Remove `do_dcache_op_by_setway()` and CLIDR/CCSIDR enumeration
- Implement `clean_D_PoU()`, `cleanInvalidate_D_PoC()`, `cleanInvalidate_L1D()` using VA-range loops with `dc civac`

**Justification - ARM Architecture**:

From ARM ARM D4.4.1 (Cache Maintenance Operations):
> "Set/way based operations operate on caches private to the PE... They do not affect caches external to the PE."

This means `dc cisw` only affects the local core's L1/L2 caches, NOT:
- System-level caches (L3, SLC)
- Caches on other cores
- Hardware coherent interconnect state

**Justification - Linux kernel removal (2015)**:

From [commit ef4f9c6b8](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=5e5f6dc13bf4):
> "arm64: kill flush_cache_all()"
> "The documented semantics of flush_cache_all are not possible to provide for arm64 (short of flushing the entire physical address space by VA)."
> — Mark Rutland, ARM Ltd

**Justification - seL4 project docs**:

From `docs/reference/tegra-whole-cache-operations.md`:
> "dc cisw (clean/invalidate by set/way) is broken on Tegra Xavier/Orin... But this is actually an architectural limitation of ALL modern ARM64 SoCs."

---

### Change 2: Use `dc civac` (PoC) for whole-cache operations

**What changes**:
- Both `clean_D_PoU()` and `cleanInvalidate_D_PoC()` use `dc civac` (PoC), not `dc cvau` (PoU)

**Justification - MMU page table walker operates at PoC**:

From ARM ARM D5.6.6:
> "The MMU performs translation table walks... at the Point of Coherency."

From seL4 `include/arch/arm/arch/machine.h` comment:
> "Page tables are read by the MMU hardware walker which operates at Point of Coherency (PoC). We must use dc civac instead of dc cvau to ensure zeros are visible to the MMU walker."

**Justification - SMP cross-core visibility**:

From `docs/reference/arm64-cache-maintenance-barriers-smp.md`:
> "PoU is not a 'global meeting point' across cores. PoC is the correct rendezvous for shared memory contents."

---

### Change 3: Add DSB barriers around cache maintenance

**What changes**:
```asm
dsb ishst        // before: ensure prior stores visible
dc  civac, Xt    // per cache line
dsb ish          // after: ensure maintenance complete
```

**Justification - ARM Architecture**:

From ARM ARM D7.2.1 (Data Synchronization Barrier):
> "DSB orders and waits until completion of prior memory accesses AND maintenance operations."

**Justification - Linux kernel practice**:

From `arch/arm64/mm/cache.S`:
```asm
SYM_FUNC_START(__clean_dcache_area_pou)
    dcache_line_size x2, x3
    sub     x3, x2, #1
    bic     x0, x0, x3
1:  dc      cvau, x0        // clean
    add     x0, x0, x2
    cmp     x0, x1
    b.lo    1b
    dsb     ish             // ensure completion
    ret
```

---

### Change 4: Handle two boot phases (before/after rootserver init)

**What changes**:
```c
if (rootserver.paging.end != 0) {
    /* After init_freemem: actual allocation range */
    *end = rootserver.paging.end + margin;
} else {
    /* Early boot (activate_kernel_vspace): kernel image only */
    *end = (word_t)ki_end + margin;
}
```

**Justification - Boot sequence analysis**:

| Call Point | Phase | rootserver.paging.end |
|------------|-------|----------------------|
| `init_cpu()` → `activate_kernel_vspace()` (line 197) | Early | **0** (not set) |
| After `arch_init_freemem()` (line 450) | Late | **Set** |
| `cleanInvalidateL1Caches()` (lines 590, 627) | Late | Set |

---

### Change 5: Remove Orin platform-specific cache.c

**What changes**:
- Remove `src/plat/orinagx/machine/cache.c`
- Remove `add_sources()` in `src/plat/orinagx/config.cmake`

**Justification**:

The platform-specific file was a workaround. With VA-based ops as the default for ALL ARM64, it's redundant.

---

## Elfloader Changes (Already Implemented - Document Only)

The elfloader already uses VA-based ops. The documentation will cover these existing changes.

### Elfloader Change 1: Replace `dc cisw` with `dc civac` in mmu.S

**Commit**: `21baa38` (elfloader: use dc civac for cache flush on aarch64)

**Location**: `elfloader-tool/src/arch-arm/armv/armv8-a/64/mmu.S`

**What was changed**:
- Removed the complex `dcache` macro that iterated over cache levels using CLIDR/CCSIDR
- Added `flush_dcache_range()` function using `dc civac` in a simple VA loop
- Changed `flush_dcache()` to flush specific known regions by VA

**Justification - Same as kernel**:

From ARM ARM D4.4.1:
> "Set/way based operations operate on caches private to the PE."

From Mark Rutland (ARM Ltd), Linux kernel commit 2015:
> "The documented semantics of flush_cache_all are not possible to provide for arm64."

**Code pattern**:
```asm
/* x0 = start address, x1 = end address */
flush_dcache_range:
    mrs     x2, ctr_el0
    ubfx    x2, x2, #16, #4
    mov     x3, #4
    lsl     x3, x3, x2      // cache line size
1:  dc      civac, x0
    add     x0, x0, x3
    cmp     x0, x1
    b.lo    1b
    dsb     sy
    isb
    ret
```

---

### Elfloader Change 2: Flush loaded kernel/rootserver images

**What was changed**:
- Added explicit flush of loaded kernel and rootserver images before MMU enable
- Ensures data is visible to MMU page table walker

**Justification - Loaded images must be visible at PoC**:

From ARM ARM D5.6.6:
> "The MMU performs translation table walks at the Point of Coherency."

After loading kernel/rootserver images via UEFI, they may be in CPU cache only. The MMU walker needs to see:
1. The loaded code/data
2. The page tables we just constructed

---

### Elfloader Change 3: TCU UART driver for Tegra

**Commit**: `4714dd1` (elfloader: add Tegra Combined UART (TCU) driver)

**What was changed**:
- Added TCU driver for early boot debug output on Orin AGX

**Justification**:

NVIDIA Tegra uses a mailbox-based "Combined UART" instead of traditional UART. Without this, no debug output is visible during elfloader execution on Orin.

---

## Documentation Content Outline

The comprehensive document (`docs/reference/arm64-cache-barrier-audit.md`) will contain:

### 1. Executive Summary
- Why these changes were needed
- ARM64 cache architecture overview

### 2. ARM Architecture Background
- PoU vs PoC definitions and requirements
- Set/way operation limitations (ARM ARM references)
- Barrier semantics (DSB ISH, DSB SY, etc.)

### 3. Linux Kernel Precedent
- 2015 removal of `flush_cache_all()`
- Marc Zyngier's 2023 speculative PTW patches
- Barrier patterns used in Linux ARM64

### 4. Kernel Changes Audit
- Each change with ARM architecture justification
- Code before/after comparison

### 5. Elfloader Changes Audit
- Each change with ARM architecture justification
- Boot sequence analysis

### 6. SMP Considerations
- Cross-core visibility requirements
- Barrier domain suffixes (ISH vs SY)

### 7. References
- ARM Architecture Reference Manual citations
- Linux kernel commits
- seL4 project documentation

---

## Implementation Steps

### Step 1: Replace generic ARM64 cache.c

Replace `src/arch/arm/armv/armv8-a/64/cache.c` with VA-based implementation:

**Key changes:**
- Remove set/way enumeration via CLIDR/CCSIDR
- Add `dcache_clean_va()` and `dcache_clean_invalidate_va()` helpers using `dc cvac`/`dc civac`
- Add `get_boot_flush_region()` to determine memory range to flush
- Implement `clean_D_PoU()`, `cleanInvalidate_D_PoC()`, `cleanInvalidate_L1D()` using VA range ops

**Boot region logic:**

The cache flush is called at two points in boot:
1. `activate_kernel_vspace()` in `init_cpu()` - **BEFORE** `rootserver` is initialized
2. Two calls after `arch_init_freemem()` - **AFTER** `rootserver` is initialized

So we MUST handle both cases. Rather than a silent fallback, we use explicit logic:

```c
static void get_boot_flush_region(word_t *start, word_t *end)
{
    *start = (word_t)ptrFromPAddr(physBase());

    if (rootserver.paging.end != 0) {
        /* After init_freemem: use actual allocation end + margin */
        *end = rootserver.paging.end + (2 * 1024 * 1024);
    } else {
        /*
         * Early boot (activate_kernel_vspace in init_cpu):
         * rootserver not yet initialized.
         * Flush kernel image + initial page tables only.
         */
        *end = (word_t)ki_end + (2 * 1024 * 1024);
    }
}
```

This is simpler - no arbitrary 32MB fallback. At early boot, we only flush kernel + page tables. After init_freemem, we flush up to rootserver allocations.

### Step 2: Remove Orin platform override

In `src/plat/orinagx/config.cmake`, remove:
```cmake
add_sources(
    DEP "KernelPlatformOrinAGX"
    CFILES src/plat/orinagx/machine/cache.c
)
```

### Step 3: Delete platform-specific cache.c

Delete `src/plat/orinagx/machine/cache.c` (now redundant).

### Step 4: Add documentation header

Add comprehensive comment explaining:
- Why set/way is broken (4 architectural issues)
- Linux precedent (2015 removal)
- Mark Rutland quote
- FOR-UPSTREAM tag for main seL4 consideration

## Scope Decisions

| Platform | Action |
|----------|--------|
| ARM64 (all) | Use new VA-based ops |
| ARM32 | No change (different architecture) |

**Rationale for ARM32 exclusion:**
- Different cache architecture (coprocessor ops)
- Mark Rutland quote specifically refers to ARM64
- Would require separate testing

## Testing Plan

1. Build sel4test for orinagx, tx2, qemu-arm-virt, bcm2711
2. Run sel4test on Orin AGX via autopilot
3. Run sel4test on QEMU ARM64 if possible
4. Verify ARM32 builds still work (no changes expected)

## Commit Structure

Single commit:
```
FOR-UPSTREAM: arm64: Use VA-based cache operations instead of set/way

Replace dc cisw/csw (set/way) with dc civac/cvac (VA-based) for all
ARM64 platforms. Set/way operations are architecturally broken on
modern ARM64 SoCs:

  1. Race with CPU speculation hides cache lines
  2. Not broadcast to other CPUs
  3. Don't affect system caches (only respect VA-based ops)
  4. Only work correctly with caches disabled

Linux ARM64 removed flush_cache_all() in 2015 for these reasons.

The new implementation flushes known boot memory regions by VA instead
of enumerating cache geometry. This was already working on Orin AGX
and is now the default for all ARM64 platforms.

Signed-off-by: ...
```

## Barrier Usage

**Cache maintenance barriers ARE included** in this plan. Per `arm64-cache-maintenance-barriers-smp.md`:

```asm
dsb ishst        // push prior stores out before maintenance
// loop: dc civac, Xt
dsb ish          // ensure maintenance complete
```

These barriers will be part of the new VA-based cache operations implementation.

## Out of Scope: VTTBR/Context Switch Barriers

**VTTBR switches and HCR_EL2.VM toggling are NOT part of this plan.**

| Topic | Rationale for Exclusion |
|-------|------------------------|
| DSB before VTTBR write | Different root cause (speculative PTW vs cache coherency) |
| HCR_EL2.VM bit toggling | Different code paths (`context_switch.h`, `vspace.c`) |
| Translation regime synchronization | Requires separate analysis per Marc Zyngier's 2023 patches |

**Why keep separate:**
- Linux KVM fixed speculative PTW as a **separate patch series** (April 2023, Marc Zyngier)
- Different failure modes: cache ops → stale data; VTTBR → TLB entries from freed memory
- Combining would make review harder and risk larger blast radius

**Recommended follow-up:** Create separate plan to audit:
1. `setCurrentUserVSpaceRoot()` - DSB before VTTBR write
2. `armv_contextSwitch()` - Barrier ordering vs PTW completion
3. Barrier strength: `dsb(nsh)` vs `dsb(ish)` per KVM patches

Reference: `projects/tii-sel4-vm/docs/reference/arm-speculative-ptw-research.md`

## Risk Mitigation

- **Early boot (rootserver not initialized)**: Handled explicitly - flush ki_end + 2MB margin
- **Late boot (after init_freemem)**: Use rootserver.paging.end + 2MB margin
- **Breaking other platforms**: Test on QEMU ARM64 before hardware
