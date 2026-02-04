# Upstream addrFromKPPtr Bugs in seL4 Kernel

**Status**: NOT FIXED (documented for future reference)
**Date**: 2024-12-19

## Summary

The seL4 kernel contains several uses of `addrFromKPPtr()` where `addrFromPPtr()` should be used instead. These are upstream bugs (present in seL4/master) that we have NOT fixed in our branch to avoid divergence.

## Background

- `addrFromKPPtr(ptr)`: Converts kernel ELF mapping pointer to physical address. Uses `KERNEL_ELF_BASE_OFFSET`. Only valid for addresses in `[KERNEL_ELF_BASE, KERNEL_ELF_TOP]`.

- `addrFromPPtr(ptr)`: Converts physical memory window pointer to physical address. Uses `PPTR_BASE_OFFSET`. Valid for all PPTR addresses including `.bss` section.

The global kernel page table structures (`armKSGlobalKernelPGD`, `armKSGlobalKernelPUD`, `armKSGlobalKernelPDs`, `armKSGlobalKernelPT`, `armKSGlobalUserVSpace`, `armKSGlobalLogPTE`) are in the `.bss` section, which is in the PPTR region, NOT the kernel ELF region. Therefore `addrFromPPtr()` is the correct function.

## Why It Works Anyway

On most platforms, `KERNEL_ELF_BASE_OFFSET == PPTR_BASE_OFFSET`, so both functions produce the same result. The bug only manifests on platforms where these offsets differ.

## Affected Locations

### File: `src/arch/arm/64/kernel/vspace.c`

#### 1. map_kernel_window() - 4 bugs

```c
// Line ~260: armKSGlobalKernelPUD
armKSGlobalKernelPGD[...] = pte_pte_table_new(
    addrFromKPPtr(armKSGlobalKernelPUD));  // BUG: should be addrFromPPtr

// Line ~266: armKSGlobalKernelPDs (in loop)
armKSGlobalKernelPUD[idx] = pte_pte_table_new(
    addrFromKPPtr(&armKSGlobalKernelPDs[idx][0]));  // BUG

// Line ~292: armKSGlobalKernelPDs (device window)
armKSGlobalKernelPUD[...] = pte_pte_table_new(
    addrFromKPPtr(&armKSGlobalKernelPDs[BIT(PT_INDEX_BITS) - 1][0]));  // BUG

// Line ~297: armKSGlobalKernelPT
armKSGlobalKernelPDs[...][...] = pte_pte_table_new(
    addrFromKPPtr(armKSGlobalKernelPT));  // BUG
```

#### 2. activate_kernel_vspace() - 2 bugs

```c
// Line ~582: armKSGlobalKernelPGD
setCurrentKernelVSpaceRoot(ttbr_new(0, addrFromKPPtr(armKSGlobalKernelPGD)));  // BUG

// Line ~585: armKSGlobalUserVSpace
setCurrentUserVSpaceRoot(ttbr_new(0, addrFromKPPtr(armKSGlobalUserVSpace)));  // BUG
```

#### 3. setVMRoot() - 2 bugs

```c
// Two locations where invalid VSpace root fallback is set:
setCurrentUserVSpaceRoot(ttbr_new(0, addrFromKPPtr(armKSGlobalUserVSpace)));  // BUG (x2)
```

#### 4. performPageTableInvocationMap() - 1 bug

```c
// Line ~: armKSGlobalLogPTE
cleanInvalByVA((vptr_t)armKSGlobalLogPTE, addrFromKPPtr(armKSGlobalLogPTE));  // BUG
```

## Total: 9 bugs in vspace.c

## Fixes We Applied (In Our Commits)

We fixed `addrFromKPPtr` bugs that WE introduced in our own commits:

1. **Commit 448255694** (tlb.h): Fixed `addrFromKPPtr(armKSGlobalUserVSpace)` in safe TLB invalidation helper
2. **Commit 1bcfd4966** (mode/machine.h): Fixed `pte_pte_invalid_new()` macro

These were fixed via interactive rebase to amend the original commits.

## How to Apply These Fixes (If Needed)

If seL4 fails to boot on a platform where `KERNEL_ELF_BASE_OFFSET != PPTR_BASE_OFFSET`, apply these fixes:

```diff
--- a/src/arch/arm/64/kernel/vspace.c
+++ b/src/arch/arm/64/kernel/vspace.c
@@ -257,13 +257,13 @@ BOOT_CODE void map_kernel_window(void)

     /* place the PUD into the PGD */
     armKSGlobalKernelPGD[GET_KPT_INDEX(PPTR_BASE, KLVL_FRM_ARM_PT_LVL(0))] = pte_pte_table_new(
-                                                                                 addrFromKPPtr(armKSGlobalKernelPUD));
+                                                                                 addrFromPPtr(armKSGlobalKernelPUD));

     /* place all PDs except the last one in PUD */
     for (idx = GET_KPT_INDEX(PPTR_BASE, KLVL_FRM_ARM_PT_LVL(1)); idx < GET_KPT_INDEX(PPTR_TOP, KLVL_FRM_ARM_PT_LVL(1));
          idx++) {
         armKSGlobalKernelPUD[idx] = pte_pte_table_new(
-                                        addrFromKPPtr(&armKSGlobalKernelPDs[idx][0])
+                                        addrFromPPtr(&armKSGlobalKernelPDs[idx][0])
                                     );
     }

@@ -286,12 +289,12 @@ BOOT_CODE void map_kernel_window(void)

     /* put the PD into the PUD for device window */
     armKSGlobalKernelPUD[GET_KPT_INDEX(PPTR_TOP, KLVL_FRM_ARM_PT_LVL(1))] = pte_pte_table_new(
-                                                                                addrFromKPPtr(&armKSGlobalKernelPDs[BIT(PT_INDEX_BITS) - 1][0])
+                                                                                addrFromPPtr(&armKSGlobalKernelPDs[BIT(PT_INDEX_BITS) - 1][0])
                                                                             );

     /* put the PT into the PD for device window */
     armKSGlobalKernelPDs[BIT(PT_INDEX_BITS) - 1][BIT(PT_INDEX_BITS) - 1] = pte_pte_table_new(
-                                                                               addrFromKPPtr(armKSGlobalKernelPT)
+                                                                               addrFromPPtr(armKSGlobalKernelPT)
                                                                            );

@@ -547,10 +579,10 @@ BOOT_CODE cap_t create_mapped_it_frame_cap(cap_t pd_cap, pptr_t pptr, vptr_t vpt
 BOOT_CODE void activate_kernel_vspace(void)
 {
     cleanInvalidateL1Caches();
-    setCurrentKernelVSpaceRoot(ttbr_new(0, addrFromKPPtr(armKSGlobalKernelPGD)));
+    setCurrentKernelVSpaceRoot(ttbr_new(0, addrFromPPtr(armKSGlobalKernelPGD)));

     /* Prevent elf-loader address translation to fill up TLB */
-    setCurrentUserVSpaceRoot(ttbr_new(0, addrFromKPPtr(armKSGlobalUserVSpace)));
+    setCurrentUserVSpaceRoot(ttbr_new(0, addrFromPPtr(armKSGlobalUserVSpace)));

     invalidateLocalTLB();

# In setVMRoot() - two locations:
-    setCurrentUserVSpaceRoot(ttbr_new(0, addrFromKPPtr(armKSGlobalUserVSpace)));
+    setCurrentUserVSpaceRoot(ttbr_new(0, addrFromPPtr(armKSGlobalUserVSpace)));

# In performPageTableInvocationMap():
-    cleanInvalByVA((vptr_t)armKSGlobalLogPTE, addrFromKPPtr(armKSGlobalLogPTE));
+    cleanInvalByVA((vptr_t)armKSGlobalLogPTE, addrFromPPtr(armKSGlobalLogPTE));
```

## Related

- See `include/machine.h` for `addrFromKPPtr()` and `addrFromPPtr()` definitions
- These globals are defined in `src/arch/arm/64/model/statedata.c` in `.bss` section
