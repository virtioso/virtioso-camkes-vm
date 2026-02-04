# seL4 Page Table Barrier and Cache Flush Audit

**Date**: 2025-12-27
**Platform**: NVIDIA Orin AGX (Tegra234) with ARM Cortex-A78AE
**Context**: RAS SCC Address Range Error investigation

## Executive Summary

This document captures a comprehensive audit of seL4's page table manipulation code, comparing it against Linux KVM's practices for ARM64 hypervisor mode. The audit identified multiple locations where DSB barriers or cache flushes are missing, which could contribute to speculative page table walker (PTW) issues.

## Background

### The Problem
On NVIDIA Orin AGX, seL4 experiences RAS (Reliability, Availability, Serviceability) SCC Address Range Errors. These occur when the ARM MMU's speculative page table walker accesses physical addresses below DRAM base (0x80000000).

### Root Cause
Zero-initialized PTEs have bits[47:12]=0, which the PTW interprets as physical address 0x0. Speculative walks following these entries trigger hardware errors on Tegra234.

### Linux KVM Pattern
Linux KVM uses consistent patterns for page table safety:
1. `dsb(nsh)` before modifying any PTE (waits for speculative walks to complete)
2. Write PTE
3. Cache flush (dc civac) to PoC
4. TLB invalidation if needed
5. `dsb(ish)` after TLBI

## Audit Findings

### Legend
- 🔴 **High Risk**: Active runtime path, could cause immediate issues
- 🟠 **Medium Risk**: Boot-time or less frequently triggered
- 🟢 **Good**: Properly protected
- 🧹 **Cleanup**: Code quality improvement

---

## Runtime Issues (Active User/Guest VSpaces)

### 🔴 performPageTableInvocationMap - Missing DSB Before Write

**File**: `kernel/src/arch/arm/64/kernel/vspace.c:1568-1585`

```c
static exception_t performPageTableInvocationMap(cap_t cap, cte_t *ctSlot, pte_t pte, pte_t *ptSlot)
{
    ctSlot->cap = cap;
    *ptSlot = pte;  // NO DSB before this!
    cleanInvalByVA((vptr_t)ptSlot, pptr_to_paddr(ptSlot));
    return EXCEPTION_NONE;
}
```

**Issue**: No DSB before writing the PTE. A speculative PTW could be in progress reading the old entry.

**Risk Level**: Medium - Only for NEW table mappings (remaps rejected at decode time). Old slot contains safe invalid PTE.

**Fix**:
```c
dsb();  // Wait for speculative walks to complete
*ptSlot = pte;
cleanInvalByVA((vptr_t)ptSlot, pptr_to_paddr(ptSlot));
```

---

### 🟠 performPageInvocationMap - No DSB For New Page Mappings

**File**: `kernel/src/arch/arm/64/kernel/vspace.c:1619-1654`

```c
static exception_t performPageInvocationMap(...)
{
    bool_t tlbflush_required = pte_ptr_get_valid(ptSlot);

    if (unlikely(tlbflush_required)) {
        // BBM path - properly handled with DSB
        dsb();
        *ptSlot = pte_pte_invalid_new();
        cleanInvalByVA(...);
        invalidateTLBByASIDVA(...);
        dsb();
    }

    // New mapping or after BBM - NO DSB before this!
    *ptSlot = pte;
    cleanInvalByVA(...);
}
```

**Issue**: For new mappings (slot was invalid), no DSB before write.

**Risk Level**: Low - Old slot contains safe invalid PTE, 64-bit atomic write.

---

## Boot-Time Issues (Rootserver Setup)

### 🟠 map_it_pt_cap - No Cache Flush After Linking

**File**: `kernel/src/arch/arm/64/kernel/vspace.c:405-428`

```c
static BOOT_CODE void map_it_pt_cap(cap_t vspace_cap, cap_t pt_cap)
{
    // ... walk to parent PD ...
    *(pd + GET_UPT_INDEX(vptr, ULVL_FRM_ARM_PT_LVL(2))) = pte_pte_table_new(
                                                              pptr_to_paddr(pt)
                                                          );
    // NO cache flush after this write!
}
```

**Issue**: Child PT is initialized and flushed via `init_pt_with_safe_ptes()`, but the parent entry linking to it is not flushed.

**Risk Level**: Low-Medium - Boot only, single-threaded, but entry stays in cache.

---

### 🟠 map_it_pd_cap - No Cache Flush After Linking

**File**: `kernel/src/arch/arm/64/kernel/vspace.c:480-499`

Same pattern as `map_it_pt_cap` - PD linked to PUD without cache flush.

---

### 🟠 map_it_pud_cap - No Cache Flush After Linking

**File**: `kernel/src/arch/arm/64/kernel/vspace.c:517-527`

Same pattern - PUD linked to PGD without cache flush.

---

### 🟠 map_it_frame_cap - No Cache Flush After Page PTE

**File**: `kernel/src/arch/arm/64/kernel/vspace.c:342-384`

```c
static BOOT_CODE void map_it_frame_cap(cap_t vspace_cap, cap_t frame_cap, bool_t executable)
{
    // ... walk to PT ...
    *(pt + GET_UPT_INDEX(vptr, ULVL_FRM_ARM_PT_LVL(3))) = pte_pte_4k_page_new(...);
    // NO cache flush after this write!
}
```

**Issue**: Page PTEs written without cache flush.

---

### 🟠 create_it_address_space - No Final Cache Flush

**File**: `kernel/src/arch/arm/64/kernel/vspace.c:554-614`

```c
BOOT_CODE cap_t create_it_address_space(cap_t root_cnode_cap, v_region_t it_v_reg)
{
    init_vspace_with_safe_ptes(rootserver.vspace);  // Flushed

    // Create PUDs - each calls map_it_pud_cap (not flushed)
    // Create PDs  - each calls map_it_pd_cap (not flushed)
    // Create PTs  - each calls map_it_pt_cap (not flushed)

    return vspace_cap;
    // NO comprehensive cache flush at end!
}
```

**Issue**: Individual table entries remain in CPU cache.

**Recommended Fix**: Add comprehensive flush at end:
```c
cleanCacheRange_RAM((word_t)rootserver.vspace,
                    (word_t)rootserver.vspace + BIT(seL4_VSpaceBits) - 1,
                    addrFromPPtr(rootserver.vspace));
```

---

### 🟠 activate_kernel_vspace - Only Flushes L1

**File**: `kernel/src/arch/arm/64/kernel/vspace.c:629-639`

```c
BOOT_CODE void activate_kernel_vspace(void)
{
    cleanInvalidateL1Caches();  // L1 only, not to PoC!
    setCurrentKernelVSpaceRoot(...);
    setCurrentUserVSpaceRoot(...);
    invalidateLocalTLB();
}
```

**Issue**: `cleanInvalidateL1Caches()` only flushes L1, not to Point of Coherency. Page table entries may remain in L2/L3.

---

## SMMU Driver Issues

### 🔴 smmu_cb_assign_vspace - No DSB Before Enable

**File**: `kernel/src/drivers/smmu/smmuv2.c:470-535`

```c
void smmu_cb_assign_vspace(word_t cb, vspace_root_t *vspace, asid_t asid)
{
    // Configure stage 1 or 2 translation
    smmu_write_reg32(..., SMMU_CBA2Rn(cb), reg);  // CBA2R
    smmu_write_reg32(..., SMMU_CBARn(cb), reg);   // CBAR
    smmu_write_reg32(..., SMMU_CBn_TCR, ...);     // TCR
    smmu_write_reg64(..., SMMU_CBn_TTBR0, ...);   // TTBR0
    smmu_write_reg32(..., SMMU_CBn_MAIR0, ...);   // MAIR

    // NO DSB HERE!

    // Enable translation
    reg = CBn_SCTLR_CFIE | CBn_SCTLR_CFRE | CBn_SCTLR_AFE | CBn_SCTLR_TRE | CBn_SCTLR_M;
    smmu_write_reg32(..., SMMU_CBn_SCTLR, reg);   // Enable!
}
```

**Issue**: No DSB between configuration writes and enabling translation. SMMU might see stale TTBR/TCR values when starting walks.

**Risk Level**: HIGH - Device transactions could use wrong page tables.

**Fix**:
```c
smmu_write_reg64(..., SMMU_CBn_TTBR0, ...);
smmu_write_reg32(..., SMMU_CBn_MAIR0, ...);
dsb();  // Ensure all config writes complete before enable
reg = CBn_SCTLR_CFIE | ... | CBn_SCTLR_M;
smmu_write_reg32(..., SMMU_CBn_SCTLR, reg);
```

---

### 🟠 smmu_cb_disable - No DSB Between Disable and TLBI

**File**: `kernel/src/drivers/smmu/smmuv2.c:537-544`

```c
void smmu_cb_disable(word_t cb, asid_t asid)
{
    uint32_t reg = smmu_read_reg32(..., SMMU_CBn_SCTLR);
    reg &= ~CBn_SCTLR_M;
    smmu_write_reg32(..., SMMU_CBn_SCTLR, reg);
    // NO DSB HERE!
    smmu_tlb_invalidate_cb(cb, asid);
}
```

**Issue**: No DSB between disabling translation and TLB invalidation.

**Fix**:
```c
smmu_write_reg32(..., SMMU_CBn_SCTLR, reg);
dsb();  // Ensure disable completes before TLBI
smmu_tlb_invalidate_cb(cb, asid);
```

---

### 🟠 smmu_sid_bind_cb - No DSB After Configuration

**File**: `kernel/src/drivers/smmu/smmuv2.c:546-561`

```c
void smmu_sid_bind_cb(word_t sid, word_t cb)
{
    smmu_write_reg32(SMMU_GR0_PPTR, SMMU_S2CRn(sid), reg);
    if (smmu_dev_knowledge.stream_match) {
        smmu_write_reg32(SMMU_GR0_PPTR, SMMU_SMRn(sid), reg);
    }
    // NO DSB after these writes!
}
```

**Issue**: No DSB after S2CR/SMR writes. Device transactions might use old stream mapping.

---

## Properly Protected Paths (Good Examples)

### 🟢 unmapPageTable

**File**: `kernel/src/arch/arm/64/kernel/vspace.c:1376-1416`

```c
void unmapPageTable(asid_t asid, vptr_t vptr, pte_t *target_pt)
{
    // ... find parent slot ...

    dsb();  // ✓ Wait for speculative walks
    *ptSlot = pte_pte_invalid_new();
    cleanInvalByVA((vptr_t)ptSlot, pptr_to_paddr(ptSlot));  // ✓ Flush to PoC
    invalidateTLBByASID(asid);  // ✓ TLB invalidation
}
```

---

### 🟢 unmapPage

**File**: `kernel/src/arch/arm/64/kernel/vspace.c:1418-1461`

Same correct pattern as `unmapPageTable`.

---

### 🟢 setCurrentUserVSpaceRoot

**File**: `kernel/include/arch/arm/arch/64/mode/machine.h:187-213`

```c
static inline void setCurrentUserVSpaceRoot(ttbr_t ttbr)
{
    if (config_set(CONFIG_ARM_HYPERVISOR_SUPPORT)) {
        word_t hcr;
        MRS("hcr_el2", hcr);
        MSR("hcr_el2", hcr & ~HCR_VM_BIT);  // ✓ Disable Stage 2
        dsb();
        isb();
        MSR("vttbr_el2", ttbr.words[0]);     // ✓ Write new VTTBR
        dsb();
        isb();
        MSR("hcr_el2", hcr);                 // ✓ Re-enable Stage 2
        dsb();
        isb();
    }
}
```

---

### 🟢 VSpace/PageTable Finalise

**File**: `kernel/src/arch/arm/64/object/objecttype.c:158-195`

```c
case cap_vspace_cap:
    if (final && cap_vspace_cap_get_capVSIsMapped(cap)) {
        deleteASID(...);  // TLB invalidation

        // ✓ Clear with safe PTEs
        for (word_t i = 0; i < BIT(seL4_VSpaceIndexBits); i++) {
            vspace[i] = pte_pte_invalid_new();
        }
        // ✓ Flush to PoC
        cleanInvalidateCacheRange_RAM(...);
    }
```

---

## Cleanup Items

### 🧹 Redundant memzero in PageTable Creation

**File**: `kernel/src/arch/arm/64/object/objecttype.c:526`

```c
case seL4_ARM_PageTableObject:
{
    pte_t safe_pte = pte_pte_invalid_new();
    pte_t *pt = (pte_t *)regionBase;

    memzero(pt, BIT(seL4_PageTableBits));  // REDUNDANT - overwritten below

    for (word_t i = 0; i < BIT(seL4_PageTableIndexBits); i++) {
        pt[i] = safe_pte;  // This overwrites the memzero
    }
}
```

**Fix**: Remove the `memzero()` call.

---

### 🧹 Duplicate Cache Flush in PageTable Creation

**File**: `kernel/src/arch/arm/64/object/objecttype.c:534-540`

```c
case seL4_ARM_PageTableObject:
{
    // ... safe PTE initialization ...
    cleanInvalidateCacheRange_RAM(...);  // First flush
}
// Fall through or after switch:
cleanInvalidateCacheRange_RAM(...);  // DUPLICATE flush
```

**Fix**: Remove the duplicate flush.

---

## Recommendations

### Priority 1: SMMU Driver (High Risk)
Add DSB barriers in SMMU driver:
- Before enabling SCTLR.M in `smmu_cb_assign_vspace`
- Between disable and TLBI in `smmu_cb_disable`
- After S2CR/SMR writes in `smmu_sid_bind_cb`

### Priority 2: Runtime Page Table Map
Add DSB before PTE write in `performPageTableInvocationMap`.

### Priority 3: Boot-Time Mappings
Either:
- Add `cleanInvalByVA()` after each `map_it_*_cap` function, OR
- Add comprehensive cache flush at end of `create_it_address_space`

### Priority 4: Cleanup
- Remove redundant `memzero()` in PageTable creation
- Remove duplicate cache flush

---

## References

- ARM ARM DDI 0487: R_LFHQG - Speculative page table walks
- Linux KVM: `arch/arm64/kvm/hyp/pgtable.c` - kvm_pgtable_walk()
- Linux KVM commit: "Synchronise speculative page table walks on translation regime change"
- seL4 Orin AGX debugging guide: `docs/reference/orin-agx-debugging-guide.md`

---

## Appendix: Comparison with Linux KVM

### Linux KVM Page Table Write Pattern
```c
// From arch/arm64/kvm/hyp/pgtable.c
static void kvm_set_table_pte(...)
{
    dsb(nsh);  // Wait for speculative walks
    WRITE_ONCE(*ptep, pte);
    dsb(nsh);  // Ensure write visible
}
```

### seL4 Current Pattern (Missing DSB)
```c
// From kernel/src/arch/arm/64/kernel/vspace.c
static exception_t performPageTableInvocationMap(...)
{
    // NO DSB
    *ptSlot = pte;
    cleanInvalByVA(...);
}
```

### Recommended seL4 Pattern
```c
static exception_t performPageTableInvocationMap(...)
{
    dsb();  // Wait for speculative walks
    *ptSlot = pte;
    cleanInvalByVA(...);
}
```

---

## Appendix B: Cortex-A78AE Errata Analysis

### Overview

NVIDIA Orin AGX uses Tegra234 SoC with 12x Cortex-A78AE cores. The A78AE is the "Automotive Enhanced" variant with safety features like lock-step execution. Several errata affect speculative behavior.

### Missing Errata Workarounds in seL4

seL4's `kernel/src/arch/arm/machine/errata.c` only handles Cortex-A15 erratum 773022. No Cortex-A78(AE) errata are implemented.

#### 🔴 Erratum 1941500 (A78AE) / 1941498 (A78) - Speculative Behavior

**Description**: Undocumented speculative behavior that can cause issues.

**Workaround**: Set `CPUECTLR_EL1[8] = 1`

**TF-A Implementation**:
```asm
workaround_reset_start cortex_a78, ERRATUM(1941498), ERRATA_A78_1941498
    sysreg_bit_set CORTEX_A78_CPUECTLR_EL1, CORTEX_A78_CPUECTLR_EL1_BIT_8
workaround_reset_end cortex_a78, ERRATUM(1941498)
```

**Affected Revisions**: r0p0, r1p0, r1p1 (still open)

**Note**: There was a bug in TF-A where A78AE code used `bic` (clear) instead of `orr` (set), doing the opposite of the workaround!

---

#### 🟠 Erratum 2376745 - Cache/Memory Speculation

**Workaround**: Set `ACTLR2_EL1[0] = 1`

**TF-A Implementation**:
```asm
workaround_reset_start cortex_a78, ERRATUM(2376745), ERRATA_A78_2376745
    sysreg_bit_set CORTEX_A78_ACTLR2_EL1, BIT(0)
workaround_reset_end cortex_a78, ERRATUM(2376745)
```

**Affected Revisions**: r0p0 - r1p2 (still open)

---

#### 🟠 Erratum 2395406 - Memory System

**Workaround**: Set `ACTLR2_EL1[40] = 1`

**Affected Revisions**: r0p0 - r1p2 (still open)

---

#### 🟠 Erratum 2742426 - Memory System

**Workaround**: Clear `ACTLR5_EL1[56]`, Set `ACTLR5_EL1[55]`

**TF-A Implementation**:
```asm
workaround_reset_start cortex_a78, ERRATUM(2742426), ERRATA_A78_2742426
    mrs x1, CORTEX_A78_ACTLR5_EL1
    bic x1, x1, #BIT(56)
    orr x1, x1, #BIT(55)
    msr CORTEX_A78_ACTLR5_EL1, x1
workaround_reset_end cortex_a78, ERRATUM(2742426)
```

**Affected Revisions**: r0p0 - r1p2 (still open)

---

#### 🟠 Erratum 2712574 (A78AE specific) - Non-ARM Interconnect

**Description**: Applies to system configurations that do not use ARM interconnect IP.

**Note**: NVIDIA Orin uses NVIDIA's own interconnect, not ARM's - this erratum likely applies!

---

### Recommendation

**Priority 0 (Critical)**: Implement Cortex-A78AE errata workarounds in seL4:

1. Add `kernel/src/arch/arm/machine/errata_a78.c` with:
   - CPUECTLR_EL1[8] = 1 (erratum 1941500)
   - ACTLR2_EL1[0] = 1 (erratum 2376745)
   - ACTLR2_EL1[40] = 1 (erratum 2395406)
   - ACTLR5_EL1[56] = 0, [55] = 1 (erratum 2742426)

2. These registers are implementation-defined and must be written at EL3 or EL2 during boot.

3. If running under ATF/TF-A, ensure TF-A has these errata enabled:
   - `ERRATA_A78_AE_1941500=1`
   - `ERRATA_A78_AE_2376748=1`
   - `ERRATA_A78_AE_2395408=1`
   - `ERRATA_A78_AE_2712574=1`

### Verification

Check if errata workarounds are applied by reading the registers at runtime:
```c
word_t cpuectlr;
MRS("S3_0_C15_C1_4", cpuectlr);  // CPUECTLR_EL1
if (!(cpuectlr & BIT(8))) {
    printf("WARNING: A78 erratum 1941500 workaround not applied!\n");
}
```

---

## Appendix C: References

### ARM Documentation
- ARM Architecture Reference Manual (ARM DDI 0487)
- Cortex-A78 Technical Reference Manual
- Cortex-A78AE Software Developer Errata Notice (SDEN-1707912)

### Linux/KVM
- `arch/arm64/kvm/hyp/pgtable.c` - Page table handling
- `arch/arm64/include/asm/barrier.h` - DSB definitions

### Trusted Firmware-A
- [CPU Specific Build Macros](https://trustedfirmware-a.readthedocs.io/en/latest/design/cpu-specific-build-macros.html)
- [Cortex-A78 Errata Source](https://github.com/ARM-software/arm-trusted-firmware/blob/master/lib/cpus/aarch64/cortex_a78.S)
