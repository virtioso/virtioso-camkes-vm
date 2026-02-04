# Boot Section Memory Corruption Investigation

**Date**: 2025-12-22
**Status**: Active investigation
**Related**: [RAS Error Investigation](orin-ras-error-investigation.md)

## Summary

Page table corruption occurs in memory recycled from the kernel's `.boot` section. Moving RAM start to 0x80032000 (past the .boot section) eliminates RAS errors, strongly suggesting the recycled boot memory is the source of corruption.

## Key Findings

### 1. Corruption Location

The corruption always occurs at:
- **Page Table**: Physical address **0x80003000** (pt_scan_list index 19)
- **Entry**: entry[303] at offset 0x978
- **Exact Physical Address**: **0x80003978**

This address is inside the kernel's `.boot` section:
```
.boot      00010000  PA 0x80000000..0x8000FFFF (64KB)
.boot.bss  00001270  PA 0x80010000..0x8001126F (~4.6KB)
.text      0003e930  PA 0x80012000...
```

### 2. Original Content at 0x80003978

The original instruction at this address was:
```asm
8080003978: 9400803e  bl 8080023a70 <__cyg_profile_func_exit.constprop.0>
```
This is part of `init_kernel()` - boot-time code that's recycled after kernel initialization.

### 3. Corruption Pattern

Corrupt values observed (all have bits 1:0 = 3, looking like "valid" PTEs):
```
Event    Value       Decimal   Delta
383552   0x33        51        -
383921   0x63        99        +48
384197   0x7f        127       +28
384515   0x9b        155       +28
384582   0xa3        163       +8
385032   0xcb        203       +40
385175   0xdf        223       +20
385278   0xe3        227       +4
385351   0xeb        235       +8
385599   0xff        255       +20
385667   0x107       263       +8
...
398653   0x5df       1503      (last)
```

**Key observations**:
- Values increment from 51 to 1503 over 93 corruption events
- Irregular increments (not a simple counter)
- All values have bits 1:0 = 0b11 (looks like valid PTE descriptor type)
- Corruption STOPS around event 398,653; PT scans continue OK afterward
- Pattern suggests a bitmap/flags field being modified, not a simple counter

### 4. Timeline

1. **Kernel boot**: `.boot` section executed (init_kernel, init_freemem, etc.)
2. **Boot section recycled**: Memory 0x80000000-0x8000FFFF returned to untyped pool
3. **Test execution**: Page table allocated at 0x80003000 from recycled memory
4. **Corruption begins**: ~event 383,552 (during CANCEL_BADGED_SENDS test iteration)
5. **Corruption ends**: ~event 398,653 (no more corruption detected)
6. **Test continues**: PT scans show OK for remainder of test
7. **RAS error**: Eventually occurs at event 533,865

### 5. Workaround Proof

**Moving RAM start to 0x80032000 eliminates ALL RAS errors.**

This proves the issue is specific to the first ~200KB of DRAM (where .boot section resides), not a general memory corruption issue.

## Hypothesis: External Memory Access

Something (UEFI, ATF, or hardware) continues to write to the first ~200KB of DRAM after seL4 takes over. Candidates:

### 1. UEFI Runtime Services
- UEFI marks some memory as `EfiRuntimeServicesData` which persists after ExitBootServices
- If UEFI allocated a logging/heartbeat buffer at the start of DRAM, it could continue writing

### 2. DMA Controller Not Stopped
- UEFI may have started a DMA transfer (e.g., Ethernet, USB, display)
- elfloader doesn't explicitly stop DMA controllers before jumping to kernel
- DMA would continue writing to its buffer address

### 3. BPMP (Boot Power Management Processor)
- BPMP is a separate CPU on Tegra that handles power management
- Communicates with main CPU via IPC shared memory
- Could be writing to a fixed address in DRAM
- Note: Investigation shows BPMP IPC uses SYSRAM, not DRAM

### 4. Hardware Activity
- Watchdog timer
- Hardware status register polling
- IOMMU/SMMU operations

## Evidence Against Other Hypotheses

### Cache Issues: DISPROVEN
- dc civac (clean+invalidate by VA) works correctly on Tegra
- Safe PTEs are written to DRAM and verified
- 5-second delay test: verification passes, corruption appears later

### Kernel Bug: UNLIKELY
- Corruption has irregular increment pattern (not typical of kernel data structure)
- Only happens in recycled .boot section memory
- Moving RAM start past .boot eliminates issue

### seL4 Memory Management Bug: POSSIBLE BUT UNLIKELY
- Would expect corruption in any recycled memory, not just .boot section
- Other tests use recycled memory without issues

## Technical Details

### Kernel Section Layout (from objdump)
```
Section         Size        VMA               PA
.boot           0x10000     0x8080000000      0x80000000
.boot.bss       0x1270      0x8080010000      0x80010000
.text           0x3e930     0x8080012000      0x80012000
.data           0x10        0x80800675f0      0x800675f0
.bss            0xa3f000    0x8080069000      0x80069000
```

### Memory Recycling Path
```
get_p_reg_kernel_img_boot()  -> Returns .boot section region
create_untypeds_for_region() -> Creates untyped caps from .boot memory
User space allocates         -> VSpace PT at 0x80003000
```

### PT Scan Implementation
```c
// Added to ftrace.c for debugging
void ftrace_add_kernel_pts(void);   // Add kernel PTs to scanner
int ftrace_maybe_scan_pt(void);      // Periodic PT integrity check

// PT_SCAN_CORRUPT format (7 entries):
// [0] marker, [1] pt_list_idx, [2] entry_idx, [3-6] pte_value
```

## Recommended Fix

### Option 1: Don't Recycle .boot Section on Orin AGX (Simple)
```c
// In kernel/src/kernel/boot.c or platform-specific code
#ifdef CONFIG_PLAT_ORINAGX
    // Skip recycling boot memory on Orin AGX
    // Cost: ~68KB of unusable memory
#else
    if (!create_untypeds_for_region(root_cnode_cap, false, boot_mem_reuse_reg, ...)) {
        ...
    }
#endif
```

### Option 2: Start RAM at 0x80010000 (Alternative)
- Modify DTS to declare RAM starting at 0x80010000
- Loses 64KB of RAM but simpler than code changes

### Option 3: Investigate Root Cause (Long-term)
1. Read UEFI memory map at boot to identify RuntimeServices regions
2. Check DMA controller status registers at kernel entry
3. Add IOMMU/SMMU setup to block stray DMA

## Test Results

### Test ID: 20251222-110309
- **Build**: el2-ftrace mode
- **PT scans**: 3,574 OK, 93 corrupt
- **Corruption**: All at pt[19] entry[303] (PA 0x80003978)
- **Values**: 0x33 to 0x5df (incrementing pattern)
- **RAS error**: 1 (at event 533,865)

### Ftrace Summary
```
Records: 547,187
pt_scan_ok: 3,574
pt_scan_corrupt: 93
ras_error_hdr: 1
```

## Workaround Results (2025-12-22)

### Test with Boot Section Recycling Disabled

Added `#if !defined(CONFIG_PLAT_ORIN_AGX)` guard around boot memory recycling in `kernel/src/kernel/boot.c`.

**Results:**
- **PT_SCAN_CORRUPT: 93 → 0** - Boot section corruption ELIMINATED
- **RAS errors: Still 1 per run** - Another source remains

This confirms there are **two separate issues**:
1. **Boot section corruption** - FIXED by not recycling
2. **Another RAS error source** - Still present, needs investigation

The remaining RAS error occurs during page table walks (ELR in ftrace code), suggesting speculative PTW issues as documented in [ARM Speculative PTW Research](../reference/arm-speculative-ptw-research.md).

## Next Steps

1. ~~Implement workaround (don't recycle .boot on Orin AGX)~~ DONE
2. ~~Test to confirm RAS errors eliminated~~ PARTIAL - boot section corruption fixed
3. Investigate remaining RAS error source (likely speculative PTW)

## Related Documents

- [RAS Error Investigation](orin-ras-error-investigation.md) - Main investigation log
- [Memory Layout](../reference/sel4-memory-layout-orinagx.md) - Physical memory layout
- [Tegra Cache Operations](../reference/tegra-cache-operations.md) - Cache flush issues (not the cause)
