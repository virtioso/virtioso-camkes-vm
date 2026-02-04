# ARM Speculative Page Table Walk Research

See the canonical overview in [ras-errors.md](../ras-errors.md).

**Date**: 2025-12-20
**Purpose**: Document findings from researching speculative PTW issues on ARM platforms (Linux/KVM/Xen)

This document captures publicly known issues with speculative page table walks (PTW) on ARM platforms that are directly relevant to the seL4 Orin AGX RAS error investigation.

---

## Executive Summary

Speculative page table walks have caused significant issues in Linux, KVM, and Xen on ARM platforms. The key findings:

1. **Linux KVM fixed this in April 2023** - Marc Zyngier's patch series added DSB barriers before changing translation regime registers
2. **ARMv7 had a security vulnerability** - Speculative PTW could create TLB entries from freed page table memory
3. **Xen uses empty page tables** during context switches to prevent speculative allocation
4. **ARM Architecture explicitly allows** speculative PTW to continue across exception level changes
5. **The pattern matches our seL4 issue** - Stale PTE data from previous iterations appearing in page tables

---

## 1. Linux KVM: Speculative PTW Synchronization (2023)

### The Patch Series

**Title**: "KVM: arm64: Synchronise speculative page table walks on translation regime change"
**Author**: Marc Zyngier (ARM Ltd)
**Date**: April 2023
**References**:
- [LWN Article](https://lwn.net/Articles/929006/)
- [Patch Series on lore.kernel.org](https://lore.kernel.org/linux-arm-kernel/ZDdIWIIogROyg1zD@linux.dev/T/)

### The Problem

From the patch cover letter:

> "It recently became apparent that the way we switch our EL1&0 translation regime is not entirely fool proof. On taking an exception from EL1&0 to EL2(&0), **the page table walker is allowed to carry on with speculative walks started from EL1&0 while running at EL2** (see R_LFHQG)."

> "Given that the PTW may be actively using the EL1&0 system registers, **the only safe way to deal with it is to issue a DSB before changing any of it**."

### ARM Architecture Reference

The patch references **ARM ARM rule R_LFHQG** (DDI0487I.a D8.1.5 'Out-of-context translation regimes'):

> "When taking an exception between the EL1&0 translation regime and the EL2 translation regime, the page table walker is allowed to complete the walks started from EL0 or EL1 while running at EL2. It means that **altering the system registers that define the EL1&0 translation regime is fraught with danger** unless we wait for the completion of such walk with a DSB."

### The Fix

The patch series implements:

1. **Add `dsb(nsh)` before restoring guest MMU state** - blocks speculative walks before changing registers
2. **Add `dsb(nsh)` after guest exits** - ensures walks complete before switching to host stage2
3. **Upgrade TLBI barriers from `dsb(ishst)` to `dsb(ish)`** - covers both page table updates AND PTW completion

### Barrier Composition

From the patch:

> "The composition of these requirements is:
> - ensuring that the page table updates are visible to all CPUs, for which a `dsb(ishst)` is what we need
> - completing any speculative page table walk started before we trapped to EL2, for which `dsb(nsh)` is enough
>
> The composition of these two barriers is a `dsb(ish)`."

### VHE vs nVHE

> "The VHE code is largely unaffected, thanks to the DSB in the context switch. Care is only needed in the NV case, where we can flip between two EL1 contexts without a context switch."

### Code Changes

The patch modifies:
- `nvhe/switch.c` - vCPU run synchronization
- `nvhe/tlb.c` - TLBI synchronization
- `vhe/sysreg-sr.c` - MMU update barriers
- `nvhe/mem_protect.c` - cache flush documentation

---

## 2. ARMv7 Page Table Free Race Condition

### The Patch

**Title**: "[RFC PATCH 2/2] ARMv7: Invalidate the TLB before freeing page tables"
**Reference**: [Linux ARM Kernel Archive](https://linux-arm-kernel.infradead.narkive.com/JFAnhCcS/rfc-patch-2-2-armv7-invalidate-the-tlb-before-freeing-page-tables)

### The Problem

> "Newer processors like Cortex-A15 may cache entries in the higher page table levels. These cached entries are ASID-tagged and are invalidated during normal TLB operations."

> "Because of the caching of the higher page table entries, **the processor may speculatively create a TLB entry after the level 2 page table has been freed but before the TLB invalidation**. If such speculative PTW accesses random data, it could create a **global TLB entry** that gets used for subsequent user space accesses."

### Security Impact

One developer emphasized the severity:

> "Another CPU could be doing a PTW at the same time and had already read the pmd before being cleared."

A freed page could be reallocated to another process, potentially exposing sensitive data (SSH private keys were mentioned as an example).

### The Problematic Sequence

**Before (vulnerable):**
1. Clear the PMD (page middle directory) entry
2. Flush the cache
3. Free the level 2 page table
4. Invalidate the TLB

**After (fixed):**
1. Clear the PMD entry
2. Flush the cache
3. **Invalidate the TLB** ← moved before free
4. Free the level 2 page table

### The Fix

The solution involved:
- Introducing `flush_tlb_user_page()` for TLB invalidation without VMA info
- Implementing `__pte_free_tlb()` to delay page table deallocation on ARMv6/v7
- **Batching freed pages and tables for deferred release after TLB operations complete**

---

## 3. Xen ARM: AT_SPECULATE Workaround

### The Workaround

Xen implements `ARM64_WORKAROUND_AT_SPECULATE` to prevent speculative PTW issues during context switches.

**References**:
- [Xen p2m.c on GitHub](https://github.com/Xilinx/xen/blob/master/xen/arch/arm/p2m.c)
- [Xen mailing list discussion](http://www.mail-archive.com/xen-devel@lists.xenproject.org/msg203045.html)

### Implementation

From the Xen source:

> "`ARM64_WORKAROUND_AT_SPECULATE`: p2m_save_state will set-up VTTBR to point to the **empty page-tables** to stop allocating TLB entries."

> "We need to stop AT to allocate TLBs entries because the context is partially modified. We only need the VMID for flushing the TLBs, so we can generate a new VTTBR with the VMID to flush and the **empty root table**."

### Key Insight

Xen uses **empty page tables** (not NULL!) during the transition window. This prevents speculative PTW from:
1. Accessing freed/stale page table memory
2. Creating TLB entries that point to garbage

### VTTBR Synchronization

> "`ARM64_WORKAROUND_AT_SPECULATE`: VTTBR_EL2 should be restored after all registers associated to EL1/EL0 translations regime have been synchronized. This is followed by an ISB to ensure synchronization."

### TLB Flush Sequence

The mental model for ordering:
```
1/ dsb nsh
2/ tlbi
3/ dsb nsh
4/ isb
```

---

## 4. ARM Architecture Specification

### Speculative Walks Across Exception Levels

From the [ARM Architecture Reference Manual](https://github.com/codingbelief/arm-architecture-reference-manual-for-armv8-a/blob/master/en/chapter_d4/d42_2_controlling_address_translation_stages.md):

> "When entering an Exception level, on completion of a DSB instruction, no new memory accesses using any translation table entries from a translation regime of an Exception level lower than the Exception level that has been entered will be observed by any observers..."

> "This requirement **does not imply that, on taking an exception to a higher Exception level, any translation table walks started before the exception was taken will be completed** by the time the higher Exception level is entered, and therefore **memory accesses required for such a translation table walk might, in effect, be performed speculatively**."

### ISB Requirement

From [systemonchips.com](https://www.systemonchips.com/the-necessity-of-isb-between-ttbr-modification-and-tlb-flush-in-arm-architectures/):

> "Without the ISB, the processor might still be using the old translation tables for speculative table walks, **leading to incorrect translations and potential Data Aborts**."

### Proper Synchronization Sequence

The recommended sequence for changing page tables:

1. Invalidate the translation table entry with a broadcast TLB invalidation instruction
2. Execute a DSB instruction to ensure completion of that invalidation
3. Write the new translation table entry
4. Execute a DSB instruction to ensure the new entry is visible

---

## 5. Cortex-A78/A78AE Errata

### Official Errata Notice

**Document**: Arm Cortex-A78 (MP102) Software Developer Errata Notice (SDEN-1401784)
**Reference**: [ARM Developer Portal](https://developer.arm.com/documentation/SDEN1401784/latest/)

### Known Errata (from TF-A documentation)

The following errata are documented for Cortex-A78:

| Erratum | Revisions | Status |
|---------|-----------|--------|
| ERRATA_A78_1688305 | r0p0 - r1p0 | Fixed |
| ERRATA_A78_1941498 | r0p0, r1p0, r1p1 | Fixed |
| ERRATA_A78_1951500 | r1p0, r1p1 | Fixed |
| ERRATA_A78_1821534 | r0p0, r1p0 | Fixed |
| ERRATA_A78_2376745 | All revisions | **Still Open** |
| ERRATA_A78_2395406 | All revisions | **Still Open** |
| ERRATA_A78_2772019 | All revisions | **Still Open** |
| ERRATA_A78_2779479 | All revisions | **Still Open** |

### Cortex-A78AE Specific

The Cortex-A78AE (used in NVIDIA Orin AGX) has its own errata:
- [Erratum 1941500](https://lists.trustedfirmware.org/archives/list/tf-a@lists.trustedfirmware.org/thread/P7RYUUJ44IHCMMVI64VQKREY4ADVIMNG/) - workaround discussion

### TLB Architecture

The Cortex-A78/A78AE has:
- **L1 ITLB**: 48 entries, fully associative
- **L1 DTLB**: 48 entries, fully associative
- **L2 TLB**: 1280 entries, 5-way set associative, unified

Reference: [ARM Cortex-A78 TLB Sizes](https://www.systemonchips.com/arm-cortex-a78-tlb-sizes-and-entry-formats/)

---

## 6. RAS and SError from Speculative Access

### How Speculative PTW Causes RAS Errors

When speculative PTW reads a garbage PTE with bits[47:12] pointing to an unmapped region (like PA=0), the hardware attempts to access that address. On Tegra platforms with strict RAS enforcement, this triggers:

1. **Synchronous External Abort (SEA)** - for synchronous access failures
2. **Asynchronous External Abort (SError)** - for asynchronous failures
3. **RAS interrupts** - platform-specific error reporting

### TF-A RAS Handling

From [TF-A RAS documentation](https://trustedfirmware-a.readthedocs.io/en/latest/components/ras.html):

> "Exceptions resulting from errors in Non-secure world are routed to and handled in EL3. Said errors are Synchronous External Abort (SEA), **Asynchronous External Abort (signalled as SErrors)**, Fault Handling and Error Recovery interrupts."

> "RAS nodes are components in the system capable of signalling errors to PEs through one of the notification mechanisms—SEAs, SErrors, or interrupts."

### Tegra-Specific Behavior

NVIDIA Tegra platforms (Xavier, Orin) have aggressive RAS error reporting. When speculative PTW accesses:
- PA=0x0 (NULL) - triggers RAS error
- PA below DRAM base - triggers RAS error
- Unmapped MMIO regions - triggers RAS error

This is more strict than some other ARM platforms where such accesses might silently fail or return garbage data.

---

## 7. Relevance to seL4 Orin AGX Issues

### Pattern Matching

| Observed Issue | Linux/KVM/Xen Precedent |
|----------------|------------------------|
| Stale PTE data from previous iterations | ARMv7 page table free race |
| RAS errors during VSpace switch | KVM speculative PTW during regime change |
| Garbage PTEs point to old PT addresses | Speculative PTW reading freed memory |
| Errors only with 4+ threads | Increased context switch rate exposes race |
| Bug B (PA=0 during unmap) | Missing safe PTE before TLBI |

### seL4 Status vs Linux/KVM/Xen Fixes

| Issue | Linux/KVM/Xen | seL4 |
|-------|---------------|------|
| DSB before VTTBR change | ✅ Fixed (2023) | ⚠️ Needs audit |
| Safe PTE during teardown | ✅ Fixed | ✅ Fixed (Bug B) |
| Empty page tables during switch | ✅ Xen uses empty tables | ✅ armKSGlobalUserVSpace |
| TLBI before PT memory free | ✅ Batched PT frees | ⚠️ Possible issue |
| Barrier composition (ish vs nsh) | ✅ Upgraded to dsb(ish) | ⚠️ Needs audit |

### Recommended seL4 Audit Areas

Based on Linux/KVM/Xen fixes, seL4 should audit:

1. **`setCurrentUserVSpaceRoot()`** - Add DSB before VTTBR write?
2. **`armv_contextSwitch()`** - Barrier ordering vs PTW completion
3. **PT free ordering** - Is TLBI complete before memory is reused?
4. **Barrier strength** - `dsb(nsh)` vs `dsb(ish)` for cross-core visibility

### The Critical Insight

From Marc Zyngier's patch:

> "The right thing was already done for SPE and TRBE, but **the PTW was ignored for unknown reasons** (probably because the architecture wasn't crystal clear at the time)."

seL4 may have the same gap - barriers that synchronize other agents but not the PTW.

---

## 8. References

### Primary Sources

1. **KVM Speculative PTW Patch**
   - [LWN Article](https://lwn.net/Articles/929006/)
   - [Patch Series](https://lore.kernel.org/linux-arm-kernel/ZDdIWIIogROyg1zD@linux.dev/T/)

2. **ARMv7 Page Table Free Issue**
   - [Linux ARM Kernel Archive](https://linux-arm-kernel.infradead.narkive.com/JFAnhCcS/rfc-patch-2-2-armv7-invalidate-the-tlb-before-freeing-page-tables)

3. **Xen ARM Workarounds**
   - [Xen p2m.c](https://github.com/Xilinx/xen/blob/master/xen/arch/arm/p2m.c)
   - [Xen mailing list](http://www.mail-archive.com/xen-devel@lists.xenproject.org/msg203045.html)

4. **ARM Architecture Reference**
   - [ARM ARM Translation Control](https://github.com/codingbelief/arm-architecture-reference-manual-for-armv8-a/blob/master/en/chapter_d4/d42_2_controlling_address_translation_stages.md)
   - [VTTBR_EL2 Register](https://developer.arm.com/documentation/ddi0601/latest/AArch64-Registers/VTTBR-EL2--Virtualization-Translation-Table-Base-Register)

5. **Cortex-A78 Errata**
   - [SDEN-1401784](https://developer.arm.com/documentation/SDEN1401784/latest/)
   - [TF-A CPU Macros](https://trustedfirmware-a.readthedocs.io/en/latest/design/cpu-specific-build-macros.html)

6. **RAS/SError Handling**
   - [TF-A RAS Documentation](https://trustedfirmware-a.readthedocs.io/en/latest/components/ras.html)

### Additional Resources

- [ARM64 Memory Barriers](https://duetorun.com/blog/20231007/a64-memory-barrier/)
- [ISB Between TTBR and TLBI](https://www.systemonchips.com/the-necessity-of-isb-between-ttbr-modification-and-tlb-flush-in-arm-architectures/)
- [Relaxed Virtual Memory in Armv8-A (Academic Paper)](https://www.cl.cam.ac.uk/~pes20/RelaxedVM-Arm/2203.00642.pdf)

---

## Document History

| Date | Change |
|------|--------|
| 2025-12-20 | Initial research document created |
