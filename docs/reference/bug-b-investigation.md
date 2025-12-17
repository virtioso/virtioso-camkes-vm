# Bug B Investigation: 0x0xxx Errors in Destruction/Revocation Path

## Summary

**Bug B** produces RAS errors with addresses 0x0fc0/0x0ff0 (near-NULL) during capability revocation and process destruction. These errors occur regardless of RAM location and are primarily triggered by CANCEL_BADGED_SENDS_0002 test.

## Key Facts

| Property | Value |
|----------|-------|
| Error addresses | 0x0fc0, 0x0ff0 (cache-line aligned, near NULL) |
| Primary trigger | CANCEL_BADGED_SENDS_0002 test |
| Code path (userspace) | `sel4utils_destroy_process`, `vka_cnode_revoke` |
| RAM dependency | None - occurs at any RAM base address |
| Error rate | ~30-40 errors per 100 test iterations |

## Investigation Log

### Step 1: Trace from userspace to kernel

**Userspace call chain:**
```
vka_cnode_revoke() [libsel4vka/include/vka/capops.h]
  └─> seL4_CNode_Revoke() [syscall]
```

`vka_cnode_revoke` is a thin wrapper:
```c
static inline int vka_cnode_revoke(const cspacepath_t *src)
{
    return seL4_CNode_Revoke(src->root, src->capPtr, src->capDepth);
}
```

### Step 2: Kernel CNode_Revoke implementation

**Kernel call chain:**
```
seL4_CNode_Revoke [syscall entry]
  └─> cteRevoke() [kernel/src/object/cnode.c]
        └─> cteDelete() for each child cap
              └─> finaliseSlot()
                    └─> finaliseCap()
                          └─> Arch_finaliseCap() [ARM64]
                                ├─> deleteASID()
                                ├─> unmapPageTable()
                                ├─> unmapPage()
                                └─> vcpu_finalise()
```

### Step 3: Key functions in revocation path

**cteRevoke()** - Iterates MDB children and deletes each:
```c
exception_t cteRevoke(cte_t *slot)
{
    for (nextPtr = CTE_PTR(mdb_node_get_mdbNext(slot->cteMDBNode));
         nextPtr && isMDBParentOf(slot, nextPtr);
         nextPtr = CTE_PTR(mdb_node_get_mdbNext(slot->cteMDBNode))) {
        status = cteDelete(nextPtr, true);
        // ... preemption point
    }
}
```

**Arch_finaliseCap()** - Handles ARM64 cap types:
- `cap_vspace_cap` → `deleteASID()`
- `cap_page_table_cap` → `unmapPageTable()`
- `cap_frame_cap` → `unmapPage()`
- `cap_vcpu_cap` → `vcpu_finalise()`

### Step 4: Hypothesis - Zero PTEs during unmapping

The 0x0fc0/0x0ff0 addresses suggest:
1. Speculative page table walk reads a zero-initialized PTE
2. The "address" field of a zero PTE is interpreted as physical address
3. 0x0fc0 and 0x0ff0 are the resulting garbage addresses

**Question**: Where are PTEs zeroed during revocation, and is there a race with speculative PTW?

### Step 5: Functions to investigate

1. **unmapPage()** - Clears frame PTEs
2. **unmapPageTable()** - Clears page table PTEs
3. **deleteASID()** - Deletes ASID, may affect VTTBR
4. **emptySlot()** - Clears capability slot after finalization

### Step 6: unmapPage analysis

Location: `kernel/src/arch/arm/64/kernel/vspace.c:1175`

```c
void unmapPage(vm_page_size_t page_size, asid_t asid, vptr_t vptr, pptr_t pptr)
{
    // ... lookup PTE ...

    *(lu_ret.ptSlot) = pte_pte_invalid_new();  // Line 1203: PTE cleared to ZERO
    cleanInvalByVA((vptr_t)lu_ret.ptSlot, ...); // Line 1205: dc civac
    invalidateTLBByASID(asid);                   // Line 1209: TLBI
}
```

**Race window**: Between line 1203 (PTE=0) and line 1209 (TLBI), speculative PTW on another core could read the zero PTE.

### Step 7: deleteASID - CRITICAL!

Location: `kernel/src/arch/arm/64/kernel/vspace.c:1212`

```c
void deleteASID(asid_t asid, vspace_root_t *vspace)
{
    // ...
    invalidateTLBByASID(asid);           // TLB invalidation first
    invalidateASIDEntry(asid);            // ASID entry invalidation
    poolPtr->array[ASID_LOW(asid)] = asid_map_asid_map_none_new();  // Clear pool
    setVMRoot(NODE_STATE(ksCurThread));   // <-- VTTBR SWITCH!
}
```

**CRITICAL**: `deleteASID` calls `setVMRoot()` which switches VTTBR!
This could trigger speculative PTW on the NEW page tables while OLD tables still being cleaned up.

### Step 7: TLB invalidation sequence

The `invalidateTLBByASIDVA` does:
```c
static inline void invalidateLocalTLB_IPA(word_t ipa)
{
    asm volatile("tlbi ipas2e1is, %0" :: "r"(ipa));
    asm volatile("dsb ish");
    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");
}
```

**But**: The PTE is already zero BEFORE the TLBI. Speculative PTW on another core could read zero.

### Step 8: setCurrentUserVSpaceRoot - HCR_EL2.VM workaround

Location: `kernel/include/arch/arm/arch/64/mode/machine.h:189`

```c
static inline void setCurrentUserVSpaceRoot(ttbr_t ttbr)
{
    // Workaround for Cortex-A78AE speculative PTW
    word_t hcr;
    MRS("hcr_el2", hcr);
    MSR("hcr_el2", hcr & ~HCR_VM_BIT);  // Disable Stage 2
    dsb(); isb();
    MSR("vttbr_el2", ttbr.words[0]);     // Write new VTTBR
    dsb(); isb();
    MSR("hcr_el2", hcr);                  // Re-enable Stage 2
    dsb(); isb();
}
```

**The HCR_EL2.VM workaround protects VTTBR switches** - but Bug B still occurs!

### Step 9: KEY INSIGHT - Race window in unmapPage

The HCR_EL2.VM workaround only protects VTTBR switches. But the race window in `unmapPage` is NOT during VTTBR switch:

```
unmapPage():
  1203:  *(lu_ret.ptSlot) = pte_pte_invalid_new();  // PTE = 0   ← RACE START
  1205:  cleanInvalByVA(...);                        // dc civac
  1209:  invalidateTLBByASID(asid);                  // TLBI      ← RACE END
```

**Problem**: Between lines 1203 and 1209, Stage 2 is ENABLED and another core can speculatively walk the page tables and read the ZERO PTE!

**The HCR_EL2.VM workaround doesn't help here** because:
1. Stage 2 remains enabled during unmapPage
2. The speculative PTW can happen on OTHER cores
3. The zero PTE is visible to speculative walkers before TLBI completes

### Step 10: CONFIRMED - pte_pte_invalid_new() returns ALL ZEROS

From `kernel/include/arch/arm/arch/64/mode/object/structures.bf`:

```c
block pte_invalid {
    padding                         5
    field pte_sw_type               1    // = 0
    padding                         56   // all zeros
    field pte_hw_type               2    // = 0
}

tagged_union pte pte_type(pte_hw_type, pte_sw_type) {
    tag pte_invalid             (0, 0)   // Both type fields = 0
}
```

**CONFIRMED**: `pte_pte_invalid_new()` returns a 64-bit value with ALL BITS ZERO!

**This explains the error addresses:**
- When speculative PTW reads the zero PTE, bits 12-47 (page_base_address) = 0
- The walker tries to access physical address 0x0000
- Cache-line alignment produces addresses 0x0fc0, 0x0ff0
- These are exactly the Bug B error addresses!

### Step 11: Why cache-line offsets 0x0fc0 and 0x0ff0?

ARM64 cache lines are 64 bytes (0x40). The addresses 0x0fc0 and 0x0ff0:
- 0x0fc0 = 0x0f * 0x40 + 0x0 (line 15 of a 4KB page)
- 0x0ff0 = 0x0f * 0x40 + 0x30 (line 15, offset 0x30)

These likely correspond to specific fields within page table structures being speculatively accessed.

### Step 12: Potential Fix Strategies

1. **Disable Stage 2 during PTE clearing** - similar to VTTBR switch workaround
2. **Use non-zero invalid PTE pattern** - so speculative walks hit safe memory
3. **Reorder: TLBI first, then clear PTE** - ARM BBM (Break-Before-Make) issues
4. **Full ASID flush before clearing** - more expensive but safer

---

## Step 13: Linux/KVM Break-Before-Make Analysis

Linux KVM enforces ARM's break-before-make (BBM) requirement for stage-2 page tables:

1. **Clear old entry first** - `kvm_set_pte(pte, __pte(0))`
2. **TLB invalidate** - `kvm_tlb_flush_vmid_ipa(kvm, addr)`
3. **Write new entry** - `kvm_set_pmd(pmd, *new_pmd)`

**However**: Linux/KVM has the SAME race window! The difference is:
- KVM's invalid PTE is __pte(0) - same as seL4!
- KVM minimizes the window but doesn't eliminate it
- KVM relies on memory being unmapped from guest during the window

**Key insight from KVM patch (Oliver Upton):**
> "The break-before-make sequence is a bit annoying as it opens a window wherein memory is unmapped from the guest. KVM should replace the PTE as quickly as possible and avoid unnecessary work in between."

KVM's approach: defer cleanup to AFTER installing new PTE to minimize the window.

### Step 14: TF-A Errata Analysis

Checked `/home/hlyytine/pkvm/Linux_for_Tegra/source/tegra/optee-src/atf/arm-trusted-firmware/plat/nvidia/tegra/soc/t234/platform_t234.mk`:

The Orin AGX enables **ERRATA_A78_AE_2466780** which says:
> "This can be avoided by flushing the mop cache following a write to registers SCR_EL3, **HCR_EL2**, or SCTLR_ELx"

This erratum is applied via CPU patch registers during reset - it makes the CPU automatically flush the micro-op cache after HCR_EL2 writes. **This should make the HCR_EL2.VM workaround more reliable**.

**But this doesn't help Bug B** because:
- Bug B occurs when Stage 2 is ENABLED (HCR_EL2.VM = 1)
- The race is in PTE clearing, not in HCR_EL2 manipulation
- Speculative PTW can happen on any core while Stage 2 is active

### Step 15: seL4 Already Has Partial Workaround!

Found in `kernel/include/arch/arm/armv/armv8-a/64/armv/tlb.h`:

```c
static inline void invalidateLocalTLB_VMID(word_t vmid)
{
    /* ... */
    /* We need to switch to the target VMID for flushing
     * the TLB if necessary.
     * Note: We use armKSGlobalUserVSpace as a valid empty page table base.
     * Using base=0 causes RAS errors on some platforms (e.g., Tegra234/Orin)
     * where the hardware does speculative table walks during TLBI.
     */
    if (v != vmid) {
        setCurrentUserVSpaceRoot(ttbr_new(vmid, addrFromKPPtr(armKSGlobalUserVSpace)));
    }
    /* ... */
}
```

**seL4 already knows about speculative PTW causing RAS errors during TLBI!**
They use `armKSGlobalUserVSpace` (valid page tables) instead of 0 for VTTBR base.

**The pattern is similar for PTEs** - but PTEs are still set to all zeros.

### Step 16: Why Linux doesn't trigger this (LIKELY ANSWER)

Linux also uses zero for invalid PTEs (`__pte(0)`), yet doesn't seem to trigger RAS errors. Here's the likely explanation:

| Aspect | seL4 Hypervisor | Linux (no KVM) | Linux + KVM |
|--------|-----------------|----------------|-------------|
| Stage 2 usage | **ALL processes** | None | Only VM guests |
| When Stage 2 PTEs cleared | Every process destruction | Never | Only VM destruction |
| Frequency of race opportunity | **Very high** | Zero | Low |

**Key insight**: seL4 in hypervisor mode uses Stage 2 translation for ALL userspace processes. Every `vka_cnode_revoke` call during test cleanup triggers Stage 2 PTE clearing. Native Linux never touches Stage 2 tables during normal process destruction - it only uses Stage 1.

**Linux KVM** does use Stage 2 for VM guests, but:
1. VM destruction is infrequent compared to seL4 process operations
2. KVM optimizes with deferred TLBI and range-based instructions (FEAT_TLBIRANGE)
3. The race window probability is much lower

**To validate**: Stress-test KVM VM creation/destruction loop on Orin and check for RAS errors. If KVM also triggers errors, it confirms the Stage 2 speculation issue.

**Additional factors:**
- seL4's HCR_NATIVE includes HCR_DC (default cacheable) - might affect speculation
- seL4's capability revocation clears many PTEs rapidly in succession
- seL4's TLBI sequences might differ from KVM's optimized paths

### Step 17: Potential Fix Analysis

| Fix Strategy | Pros | Cons |
|--------------|------|------|
| **Disable Stage 2 during PTE clearing** | Similar to existing VTTBR workaround | Performance impact, need to disable on ALL cores |
| **Non-zero invalid PTE pattern** | Simple change, safe address | Need to ensure the address is always safe memory |
| **TLBI first, then clear** | Eliminates race | Violates ARM BBM rules, may cause other issues |
| **IPI to stop other cores** | Complete safety | Major performance hit, complex |

### Step 18: PROPOSED FIX - Safe Invalid PTE Pattern

**Discovery**: seL4 already has partial fixes in place:
- `objecttype.c:467` - VSpaceObject initialized with safe invalid PTEs ✓
- `objecttype.c:505` - PageTableObject initialized with safe invalid PTEs ✓
- `vspace.c:314` - armKSGlobalUserVSpace initialized with safe invalid PTEs ✓

**Missing piece**: `unmapPage()` and `unmapPageTable()` still use `pte_pte_invalid_new()` which returns all zeros!

**Proposed fix**: Add a helper function for safe invalid PTEs:

```c
/* In kernel/src/arch/arm/64/kernel/vspace.c or a header */
/**
 * Create a "safe" invalid PTE for Tegra234/Orin.
 * Bits[47:12] contain a safe physical address (armKSGlobalUserVSpace)
 * Bit[0] = 0 means invalid (no translation occurs)
 * This prevents speculative PTW from generating accesses to PA=0.
 *
 * NOTE: This applies to BOTH hypervisor (EL2) and non-hypervisor (EL1) modes.
 * Testing on 2025-12-17 confirmed speculative PTW occurs at ALL exception
 * levels - ARM_HYP=OFF produced ~200x MORE errors than ARM_HYP=ON when
 * safe PTEs were only enabled for EL2.
 */
static inline pte_t pte_safe_invalid_new(void)
{
    pte_t pte;
    paddr_t safe_pa = addrFromKPPtr(armKSGlobalUserVSpace);
    pte.words[0] = safe_pa & 0xfffffffff000ull;
    return pte;
}
```

**Files to modify**:

1. **kernel/src/arch/arm/64/kernel/vspace.c:1169** (unmapPageTable):
   ```c
   // Change from:
   *ptSlot = pte_pte_invalid_new();
   // To:
   *ptSlot = pte_safe_invalid_new();
   ```

2. **kernel/src/arch/arm/64/kernel/vspace.c:1203** (unmapPage):
   ```c
   // Change from:
   *(lu_ret.ptSlot) = pte_pte_invalid_new();
   // To:
   *(lu_ret.ptSlot) = pte_safe_invalid_new();
   ```

**Why this works**:
- The PTE is still "invalid" (bit[0]=0, bit[1]=0)
- Hardware won't use it for actual translation
- BUT speculative PTW will see a safe PA in bits[47:12]
- Instead of accessing PA=0 (causing RAS error), it speculatively accesses armKSGlobalUserVSpace
- armKSGlobalUserVSpace is valid kernel memory - safe to speculatively read

---

## Step 19: FIX IMPLEMENTED AND VERIFIED

**Fix committed to**: `kernel/src/arch/arm/64/kernel/vspace.c`

Added `pte_safe_invalid_new()` function and updated:
- `unmapPageTable()` line 1201
- `unmapPage()` line 1237

**Test Results:**

| Test Run | 0x0fc0/0x0ff0 Errors (Bug B) | 0x7fffxxxx Errors (Bug A) |
|----------|------------------------------|---------------------------|
| Before fix (20251217-02xxxx) | 56-92 per run | Present |
| **After fix (20251217-111530)** | **0 per run** | Present |

**BUG B IS FIXED!** Zero 0x0xxx errors in 5 consecutive runs.

The remaining errors are all Bug A (0x7fffxxxx - FPU/syscall path), which requires separate investigation.

---

## Current Status

**BUG B: FIXED ✓**

Root cause was `pte_pte_invalid_new()` returning all zeros. Fix uses `pte_safe_invalid_new()` which sets bits[47:12] to `armKSGlobalUserVSpace` PA while keeping bits[1:0]=0 (invalid).

**BUG A: Still present** - the 0x7fffxxxx errors in FPU/syscall path need separate investigation.

---

## Related Code Locations

| Function | File | Line |
|----------|------|------|
| vka_cnode_revoke | libsel4vka/include/vka/capops.h | ~104 |
| cteRevoke | kernel/src/object/cnode.c | ~540 |
| cteDelete | kernel/src/object/cnode.c | ~553 |
| finaliseSlot | kernel/src/object/cnode.c | ~615 |
| Arch_finaliseCap | kernel/src/arch/arm/64/object/objecttype.c | ~139 |
| unmapPage | kernel/src/arch/arm/64/kernel/vspace.c | ? |
| unmapPageTable | kernel/src/arch/arm/64/kernel/vspace.c | ? |
| deleteASID | kernel/src/arch/arm/64/kernel/vspace.c | ? |
