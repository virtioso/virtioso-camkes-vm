# Kernel Page Table Initialization Investigation

**Date**: 2025-12-21
**Status**: Tested, did not fix the issue
**Related**: [orin-agx-debugging-guide.md](../orin-agx-debugging-guide.md)

## Summary

Investigated whether uninitialized (zero) entries in kernel page tables could cause RAS errors through speculative page table walks. Implemented self-referential safe PTEs for all kernel page tables. **Result: Did not reduce RAS errors.**

## Background

### The Zero PTE Hypothesis

Kernel page tables (`armKSGlobalKernelPGD`, `armKSGlobalKernelPUD`, `armKSGlobalKernelPDs`, `armKSGlobalKernelPT`) are declared in BSS and only partially initialized during boot:

```c
// In statedata.c - BSS-allocated (zero-initialized)
pte_t armKSGlobalKernelPGD[BIT(PT_INDEX_BITS)] ALIGN_BSS(...);
pte_t armKSGlobalKernelPUD[BIT(PT_INDEX_BITS)] ALIGN_BSS(...);
pte_t armKSGlobalKernelPDs[BIT(PT_INDEX_BITS)][BIT(PT_INDEX_BITS)] ALIGN_BSS(...);
pte_t armKSGlobalKernelPT[BIT(PT_INDEX_BITS)] ALIGN_BSS(...);
```

In `map_kernel_window()`, only specific entries are set:
- `armKSGlobalKernelPGD`: Only 1 entry (pointing to PUD)
- `armKSGlobalKernelPUD`: Only entries for kernel window
- `armKSGlobalKernelPDs`: Only entries mapping physical memory
- `armKSGlobalKernelPT`: Only device mapping entries

**All other entries remain ZERO.**

### The Speculation Chain Theory

A zero PTE has:
- Bits [47:12] = 0 → address field points to physical address 0x0
- Bits [1:0] = 0 → entry is invalid

The hypothesis was:
1. Speculative PTW reads a zero PTE
2. Even though invalid, speculation prefetches from address 0x0
3. Address 0x0 on Orin (not DRAM, could be MMIO/device region) contains garbage
4. That garbage looks like a PTE pointing to 0x7fffxxxx
5. PTW tries to access 0x7fffxxxx (below DRAM at 0x80000000)
6. **RAS Address Range Error!**

This is analogous to the fix that worked for user page tables - using `pte_pte_invalid_new()` which points to valid DRAM instead of zero.

## Implementation

### Self-Referential Safe PTEs

Each page table's entries point back to itself (valid DRAM address) with low bits = 0 (invalid):

```c
// In map_kernel_window() - added before populating real mappings
pte_t safe_pgd = { .words[0] = addrFromKPPtr(armKSGlobalKernelPGD) & 0xfffffffff000ull };
pte_t safe_pud = { .words[0] = addrFromKPPtr(armKSGlobalKernelPUD) & 0xfffffffff000ull };
pte_t safe_pt  = { .words[0] = addrFromKPPtr(armKSGlobalKernelPT)  & 0xfffffffff000ull };

for (idx = 0; idx < BIT(PT_INDEX_BITS); idx++) {
    armKSGlobalKernelPGD[idx] = safe_pgd;
}
for (idx = 0; idx < BIT(PT_INDEX_BITS); idx++) {
    armKSGlobalKernelPUD[idx] = safe_pud;
}
for (word_t i = 0; i < BIT(PT_INDEX_BITS); i++) {
    pte_t safe_pd = { .words[0] = addrFromKPPtr(&armKSGlobalKernelPDs[i][0]) & 0xfffffffff000ull };
    for (idx = 0; idx < BIT(PT_INDEX_BITS); idx++) {
        armKSGlobalKernelPDs[i][idx] = safe_pd;
    }
}
for (idx = 0; idx < BIT(PT_INDEX_BITS); idx++) {
    armKSGlobalKernelPT[idx] = safe_pt;
}
```

**Why self-referential?**
- Address points to valid DRAM (the PT itself is in kernel memory)
- Low bits = 0 means invalid entry (won't actually be walked)
- If PTW speculatively follows, it reads the same safe PT again
- No chain to garbage at address 0x0

## Test Results

**Before fix (baseline)**: 47 RAS errors (29 SCC + 18 ACI)
**After fix**: 53 RAS errors (31 SCC + 22 ACI)

**Conclusion: The fix did NOT help.** Error count slightly increased (within normal variance).

### Error Characteristics (unchanged)

Error addresses still in 0x7fffxxxx range:
```
0x7ffffec0:   12x (below DRAM, NS=1)
0x7fffff00:    6x (below DRAM, NS=1)
0x7fffffc0:    5x (below DRAM, NS=1)
...
```

Errors still occur at:
- `pte_ptr_get_pte_type` (8x) - during page table traversal
- `Arch_createObject` (8x) - during PT creation
- `scan_vspace_for_bad_ptes` (3x) - during verification

## Analysis

### Why This Didn't Work

The kernel page tables are NOT the source of the garbage 0x7fffxxxx addresses:

1. **Kernel PT entries already safe**: Even zero PTEs point to address 0x0 which is NOT 0x7fffxxxx
2. **Different translation regime**: Kernel uses TTBR1_EL2 (stage-1), errors involve VTTBR_EL2 (stage-2)
3. **Garbage is in user VSpaces**: The 0x7fffxxxx values must come from user page tables

### The 0x7fffxxxx Mystery Remains

These addresses are suspiciously consistent:
- 0x7fff = 32767 (0111_1111_1111_1111 binary)
- Just below DRAM base (0x80000000)
- NOT random garbage

Possible sources:
1. **Stale data from previous allocations** - memory reuse without proper clearing
2. **UEFI/firmware leftovers** - stale page table entries from boot
3. **Specific initialization pattern** - some code writing this value

## Related Changes

### New Ftrace Markers Added

During this investigation, added new ftrace markers for better visibility:

| Marker | Value | Purpose |
|--------|-------|---------|
| `FTRACE_RETYPE_MARKER` | 0xFFF6 | When memory is retyped to PT/VSpace |
| `FTRACE_INIT_PT_MARKER` | 0xFFF4 | PT initialization phases (START/AFTER_WRITE/AFTER_FLUSH/END_OK/END_BAD) |
| `FTRACE_CREATE_OBJ_MARKER` | 0xFFF2 | Entry to Arch_createObject |
| `FTRACE_PT_MAP_MARKER` | 0xFFF0 | When PT is mapped into parent (becomes walkable by PTW) |

### Decoder Object Type Fix

Fixed Python decoder to show correct seL4 object types for non-MCS builds:
- Type 6 = VSpace (was incorrectly shown as "Reply")
- Type 9 = PageTable

## Files Modified

| File | Change |
|------|--------|
| `kernel/src/arch/arm/64/kernel/vspace.c` | Added self-referential safe PTE init in `map_kernel_window()` |
| `kernel/src/benchmark/ftrace.c` | Added `ftrace_pt_map()` function |
| `kernel/include/benchmark/ftrace.h` | Added PT_MAP marker definitions |
| `kernel/tools/decode_ftrace_binary.py` | Added PT_MAP parsing, fixed object type names |

## Conclusions

1. **Kernel page table zero entries are NOT the cause** of 0x7fffxxxx RAS errors
2. **Self-referential safe PTEs** are a valid safety measure but don't fix this bug
3. **The garbage must come from user VSpaces** or stage-2 translation tables
4. **The 0x7fffxxxx pattern needs investigation** - where does this specific value originate?

## Next Steps

1. Investigate user VSpace destruction/recycle paths
2. Check if 0x7fffxxxx appears in any seL4 code as a marker value
3. Look at ASID/VMID recycling and TLBI timing
4. Analyze memory state during CANCEL_BADGED_SENDS test iterations
