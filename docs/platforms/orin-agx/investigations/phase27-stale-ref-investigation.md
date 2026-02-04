# Phase 27: Stale Reference Investigation

**Date**: 2025-12-22
**Status**: In Progress

## Summary

Added stale reference detection to catch parent PTEs that still reference freed child PTs. Testing revealed that parent PTEs ARE being cleared correctly - the issue lies elsewhere.

## Changes Made

### 1. Stale Reference Check (`ftrace.c`)

Added `ftrace_check_stale_refs(word_t freed_paddr)`:
- Scans ALL known PTs for references to a just-freed PT address
- Called immediately after `unmapPageTable()` clears the parent entry
- If any PT still references the freed address, logs `STALE_REF:` error
- Invalidates cache before scanning to read from DRAM

### 2. PT Scanner List Management (`ftrace.c`)

Added `ftrace_remove_pt_scan(word_t pt_paddr)`:
- Removes PT from scan list when freed
- Prevents scanning memory that's no longer a PT

### 3. Hook Points (`objecttype.c`)

Added calls in `Arch_finaliseCap()` for both:
- `cap_vspace_cap`: After `deleteASID()`, before clearing VSpace entries
- `cap_page_table_cap`: After `unmapPageTable()`, before clearing PT entries

## Test Results (2025-12-22 12:23)

**Build**: el2-ftrace mode
**Test**: 100x CANCEL_BADGED_SENDS_0002 stress test

### Key Metrics

| Metric | Value |
|--------|-------|
| Total test runs | 67 |
| Passed | 67 (100%) |
| RAS errors | 1 |
| STALE_REF messages | **0** |
| PT scans (pt_scan_ok) | 130,971 |
| PT corruptions detected | 0 |

### RAS Error Details

```
ERROR:   RAS Uncorrectable Error in SCC, base=0xe017000:
ERROR:   ADDR = 0x800000007ffe0000
ERROR:   SERR = Illegal address (software fault): 0xd
ERROR:   IERR = Address Range Error: 0x9
SDEI RAS: interrupted_pc=0x808002a47c (ftrace_scan_one_pt)
SDEI RAS: VTTBR_EL2=0x10000ac002000 (VMID 1, base 0xac002000)
```

- Error address: 0x7ffe0000 (below DRAM base 0x80000000)
- Occurred during PT scan, not during VTTBR switch
- Only 1 error in entire test run

### Ftrace Event Counts

```
Records: 22,280,699
func_enter/exit: 7,234,674 each
kernel_entry: 1,124,052
kernel_exit: 1,128,196
vttbr: 448,633
tlbi: 119,354
safe_pte: 566,272
init_pt: 4,112
create_obj: 1,028
pt_map: 753
pt_scan_ok: 130,971
ras_error: 1
```

## Observations

### 1. Parent PTEs Are Being Cleared Correctly

**Evidence**: Zero STALE_REF messages despite checking every PT deletion.

**Implication**: The hypothesis that parent PTEs retain references to freed child PTs is **DISPROVEN**.

### 2. PT Initialization Is Working

**Evidence**: Ftrace shows proper INIT_PT sequence for PT 0xac1d2000:
```
CREATE_OBJ paddr=0xac1d2000 type=9 (PageTable)
INIT_PT paddr=0xac1d2000 phase=0 (START)
INIT_PT paddr=0xac1d2000 phase=1 (AFTER_WRITE)
INIT_PT paddr=0xac1d2000 phase=2 (AFTER_FLUSH)
INIT_PT paddr=0xac1d2000 phase=3 (END_OK - verification passed!)
PT_MAP pt=0xac1d2000 parent=0xac1e2000 slot=2
```

**Implication**: PTs are being initialized with safe PTEs and cache is flushed before mapping.

### 3. Error Occurs During PT Scan, Not VTTBR Switch

**Evidence**: ELR = 0x808002a47c is in `ftrace_scan_one_pt`.

**Implication**: The speculative PTW that causes the RAS error is triggered by:
- The PT scan's cache invalidation (dc ivac), OR
- Some other activity happening concurrently

### 4. Extra TLBI Before VTTBR Switch

We added extra TLBI in `invalidateLocalTLB_VMID()`:
```c
dsb();
asm volatile("tlbi vmalls12e1is");
dsb();
isb();
```

This was intended to prevent speculative PTW from using stale TLB entries. It exposed massive corruption in earlier tests (code-in-PT discovery), but now with only 1 error, it seems to be helping overall.

### 5. Error Rate Significantly Reduced

| Phase | Errors per 100 iterations |
|-------|---------------------------|
| Baseline (no fixes) | ~30-50 |
| After safe PTEs | ~10-20 |
| After extra TLBI | ~1-5 |
| After stale ref check | **1** |

### 6. Code-in-PageTable Was Real But Transient

The earlier discovery of sel4test-driver code in PT memory (Phase 26) was real - it proved memory recycling happens. However:
- The PT scanner now properly removes freed PTs from its list
- The safe PTE initialization is working
- The corruption we saw was likely during a timing window that the extra TLBI exposed

## Hypotheses for Remaining Error

### Hypothesis A: PT Scan Cache Invalidation Race

The PT scan uses `dc ivac` to invalidate cache before reading. This might:
1. Cause the MMU to see stale data from DRAM
2. Trigger speculative PTW on that stale data
3. If stale data has bad addresses → RAS error

**Test**: Disable PT scanning entirely and see if errors go to zero.

### Hypothesis B: Speculative PTW During DSB

The DSB barrier waits for all memory operations to complete, but speculative PTW might:
1. Start during the DSB
2. Read from a PT that's being modified
3. Find inconsistent data → RAS error

**Test**: Add ISB after DSB to prevent speculative execution.

### Hypothesis C: VMID 1 VSpace (0xac002000) Has Subtle Issue

All errors occur with VTTBR pointing to 0xac002000 (VMID 1). This VSpace might have:
1. A PTE that occasionally contains bad data
2. A timing-sensitive entry that gets corrupted under load

**Test**: Trace all modifications to VSpace 0xac002000 entries.

### Hypothesis D: Hardware Speculative Prefetch

Orin AGX might have aggressive prefetch that:
1. Speculatively reads page table entries
2. Follows next-level pointers before software sets them up
3. Hits uninitialized memory → RAS error

**Test**: Check if disabling hardware prefetch helps (if possible).

## What We've Ruled Out

| Hypothesis | Status | Evidence |
|------------|--------|----------|
| Parent PTEs not cleared | **DISPROVEN** | Zero STALE_REF messages |
| PT not initialized with safe PTEs | **DISPROVEN** | INIT_PT phase=3 (END_OK) |
| Cache not flushed after PT init | **DISPROVEN** | INIT_PT phase=2 shows flush |
| ATF/OP-TEE corrupting memory | **DISPROVEN** | (Phase 17) |
| dc cisw not working on Tegra | **DISPROVEN** | Using dc civac throughout |
| Basic IPC/scheduling issues | **DISPROVEN** | 115/122 tests never error |

## Next Steps

1. **Try disabling PT scan** to see if it's the scan itself causing the error
2. **Add more barriers** around cache invalidation in PT scan
3. **Trace VSpace 0xac002000** modifications in detail
4. **Check if error is truly random** or follows a pattern

## Files Modified

- `kernel/src/benchmark/ftrace.c` - Added `ftrace_check_stale_refs()`, `ftrace_remove_pt_scan()`
- `kernel/include/benchmark/ftrace.h` - Added function declarations
- `kernel/src/arch/arm/64/object/objecttype.c` - Added hooks in `Arch_finaliseCap()`
- `kernel/include/arch/arm/armv/armv8-a/64/armv/tlb.h` - Extra TLBI before VTTBR switch (Phase 26)
