# 200KB Diagnostic Region Investigation

**Date:** 2025-12-20
**Status:** Complete
**Result:** Diagnostic region NOT corrupted; PT corruption occurs in middle of DRAM

## Background

Previous investigations showed that moving RAM start from 0x80000000 to 0x80032000 (200KB offset) eliminated RAS errors during CANCEL_BADGED_SENDS tests. This raised the question: is something specifically corrupting the first 200KB of DRAM?

## Hypothesis

If the RAS errors are caused by something writing garbage to the beginning of DRAM (0x80000000-0x80032000), we should be able to detect this by:
1. Initializing that region with a known pattern before kernel boot
2. Periodically verifying the pattern during test execution
3. If corruption is detected, capture ftrace data to identify the culprit

## Implementation

### Overview

| Component | Change |
|-----------|--------|
| IMAGE_START_ADDR | Changed to 0x80032000 so elfloader/kernel start after diagnostic region |
| DTS memory | Changed to start at 0x80032000 (makes 0x80000000-0x80032000 device untyped) |
| Elfloader | Initializes 200KB pattern at 0x80000000 before kernel boot |
| Kernel ftrace | Scans diagnostic region every 1000 function calls |

### File Changes

#### 1. `tools/seL4/cmake-tool/helpers/application_settings.cmake`

Changed IMAGE_START_ADDR for Orin AGX from 0x80010000 to 0x80032000:

```cmake
if(KernelPlatformOrinAGX)
    # RAS diagnostic: Reserve 200KB at DRAM start for corruption detection
    set(IMAGE_START_ADDR
        0x80032000
        CACHE INTERNAL "" FORCE
    )
endif()
```

#### 2. `kernel/tools/dts/orinagx.dts`

Changed memory region to start at 0x80032000:

```dts
memory@80032000 {
    device_type = "memory";
    /* 768MB - 200KB RAM starting at 0x80032000.
     * First 200KB (0x80000000-0x80032000) becomes device untyped
     * for RAS diagnostic pattern verification. */
    reg = <0x0 0x80032000 0x0 0x2ffce000>;
};
```

This causes the kernel to treat 0x80000000-0x80032000 as device memory (not RAM), creating device untypeds for that region.

#### 3. `tools/seL4/elfloader-tool/src/arch-arm/sys_boot.c`

Added include for image_start_addr.h:

```c
#include "image_start_addr.h"
```

Added pattern initialization in `continue_boot()` (after relocation, before MMU enable):

```c
/* Initialize diagnostic region with known pattern.
 * This 200KB region at DRAM start (0x80000000-0x80032000) is used when
 * IMAGE_START_ADDR == 0x80032000 (Orin AGX RAS diagnostic build).
 * The region is periodically verified by ftrace to detect memory corruption.
 */
#if IMAGE_START_ADDR == 0x80032000UL
    {
        #define DIAG_REGION_START  0x80000000UL
        #define DIAG_REGION_SIZE   0x32000UL  /* 200KB = 204800 bytes */
        #define DIAG_PATTERN       0xDEADBEEFCAFEBABEUL

        printf("Initializing 200KB diagnostic region at 0x%lx...\n", DIAG_REGION_START);
        uint64_t *diag_p = (uint64_t *)DIAG_REGION_START;
        for (word_t diag_i = 0; diag_i < DIAG_REGION_SIZE / sizeof(uint64_t); diag_i++) {
            diag_p[diag_i] = DIAG_PATTERN ^ diag_i;  /* Pattern includes position */
        }
        /* Flush to DRAM using dc civac (clean and invalidate) */
        for (word_t diag_addr = DIAG_REGION_START;
             diag_addr < DIAG_REGION_START + DIAG_REGION_SIZE;
             diag_addr += 64) {
            asm volatile("dc civac, %0" : : "r"(diag_addr) : "memory");
        }
        asm volatile("dsb sy" ::: "memory");
        printf("Diagnostic region initialized with pattern.\n");

        #undef DIAG_REGION_START
        #undef DIAG_REGION_SIZE
        #undef DIAG_PATTERN
    }
#endif
```

#### 4. `kernel/src/benchmark/ftrace.c`

Added diagnostic region variables:

```c
#if defined(CONFIG_PLAT_ORINAGX)
/* Diagnostic region verification - 200KB at DRAM start */
#define DIAG_REGION_START  0x80000000UL
#define DIAG_REGION_SIZE   0x32000UL  /* 200KB = 204800 bytes */
#define DIAG_PATTERN       0xDEADBEEFCAFEBABEUL
#define DIAG_SCAN_INTERVAL 1000  /* Check every 1000 function calls */
static word_t diag_scan_counter = 0;
static int diag_corruption_detected = 0;
#endif
```

Added scan call in `__cyg_profile_func_enter()`:

```c
#if defined(CONFIG_PLAT_ORINAGX)
    /* Diagnostic region scan - every DIAG_SCAN_INTERVAL calls */
    if (++diag_scan_counter >= DIAG_SCAN_INTERVAL) {
        diag_scan_counter = 0;
        ftrace_scan_diagnostic_region();
    }
#endif
```

Added verification function:

```c
#if defined(CONFIG_PLAT_ORINAGX)
__attribute__((__no_instrument_function__))
static void ftrace_scan_diagnostic_region(void)
{
    if (diag_corruption_detected) return;

    uint64_t *p = (uint64_t *)ptrFromPAddr(DIAG_REGION_START);
    word_t bad_count = 0;
    word_t first_bad_idx = 0;
    uint64_t first_bad_val = 0;
    uint64_t first_expected = 0;

    /* Invalidate cache to read from DRAM */
    for (word_t addr = (word_t)p; addr < (word_t)p + DIAG_REGION_SIZE; addr += 64) {
        asm volatile("dc ivac, %0" : : "r"(addr));
    }
    asm volatile("dsb sy" ::: "memory");

    /* Check pattern */
    word_t count = DIAG_REGION_SIZE / sizeof(uint64_t);
    for (word_t i = 0; i < count; i++) {
        uint64_t expected = DIAG_PATTERN ^ i;
        if (p[i] != expected) {
            if (bad_count == 0) {
                first_bad_idx = i;
                first_bad_val = p[i];
                first_expected = expected;
            }
            bad_count++;
        }
    }

    if (bad_count > 0) {
        diag_corruption_detected = 1;
        printf("\n!!! DIAG REGION CORRUPTED at call %lu !!!\n", ftrace_total_logged);
        printf("  %lu of %lu words corrupted\n", bad_count, count);
        printf("  First bad: [%lu] = 0x%lx (expected 0x%lx)\n",
               first_bad_idx, first_bad_val, first_expected);
        printf("  Address: 0x%lx\n", DIAG_REGION_START + first_bad_idx * 8);
        ftrace_dump_binary();
    }
}
#endif
```

### Technical Notes

**Why ptrFromPAddr(0x80000000) works even when RAM starts at 0x80032000:**

On ARM64, the kernel maps all physical addresses (0 to PADDR_TOP) into the kernel virtual address window. The mapping is:
- `PADDR_BASE = 0` (for ARM64)
- `PPTR_BASE_OFFSET = PPTR_BASE - PADDR_BASE = 0x8000000000`
- `ptrFromPAddr(0x80000000) = 0x8080000000`

This virtual address is within the kernel window and correctly maps to physical 0x80000000, regardless of where "RAM" officially starts according to the DTS.

**Why device untypeds are created for 0x80000000-0x80032000:**

The kernel's `create_untypeds()` function in `boot.c` creates device untypeds for any physical address ranges that are:
1. Below the first reserved region (kernel image)
2. Between reserved regions
3. Not covered by the DTS memory regions

Since DTS memory starts at 0x80032000, the range 0x80000000-0x80032000 falls into category 1 and becomes device untypeds.

## Test Results

**Build:** `mcp__sel4-autopilot__build_sel4test mode=el2-ftrace`
**Test ID:** 20251220-094313

### Boot Output

```
ELF-loading image 'kernel' to 80032000
  paddr=[80032000..80ec9fff]
  vaddr=[8080032000..8080ec9fff]
  virt_entry=8080032000
ELF-loading image 'rootserver' to 80ecb000
Initializing 200KB diagnostic region at 0x80000000...
Diagnostic region initialized with pattern.
Enabling hypervisor MMU and paging
Jumping to kernel-image entry point...
```

### Memory Layout

```
available phys memory regions: 1
  [80032000..b0000000)

List of untypeds (device memory at DRAM start):
0x80000000 | 17 | 1   (128KB device untyped)
0x80020000 | 16 | 1   (64KB device untyped)
0x80030000 | 13 | 1   (8KB device untyped)
                      Total: 200KB device untypeds

RAM untypeds start at:
0x80032000 | 13 | 0   (first RAM untyped)
```

### Test Summary

| Metric | Value |
|--------|-------|
| Tests run | 7 (CANCEL_BADGED_SENDS_0002 x7) |
| Tests passed | 7 (100%) |
| RAS errors | 0 |
| Diagnostic region corruptions | 0 |
| PT corruptions detected | 4 |

### PT Corruption Events

| Event | PT Address | Call Count | Corrupted Entry | Garbage Value |
|-------|------------|------------|-----------------|---------------|
| 1 | 0xac25c000 | 11,230,000 | [282] | 0x10011ea5 (addr 0x10011000) |
| 2 | 0xac234000 | 30,890,000 | - | - |
| 3 | 0xac1e6000 | 51,550,000 | - | - |
| 4 | 0xac254000 | 72,190,000 | - | - |

Note: All corrupted addresses are in the middle of DRAM (~0xACxxxxxx), not in the diagnostic region at 0x80000000.

## Conclusions

### Hypothesis Rejected

The hypothesis that "something corrupts the first 200KB of DRAM" is **NOT supported** by the evidence:

1. **Diagnostic region remained intact** - All 25,600 64-bit words (200KB) maintained their pattern throughout the entire test run (~72 million function calls).

2. **PT corruption occurs elsewhere** - The detected corruptions are in page tables allocated in the middle of DRAM (0xACxxxxxx range), not at the beginning.

3. **Zero RAS errors** - With RAM starting at 0x80032000, no RAS errors occurred despite PT corruption being detected.

### New Insights

1. **PT corruption is real but benign with 0x80032000 RAM start** - Tests pass despite corruption being detected, suggesting either:
   - Corrupted PTEs aren't accessed before being overwritten
   - The corruption detection races with normal operations
   - The corruption only affects unmapped/unused PT entries

2. **RAM start address affects RAS error manifestation** - The same underlying issue (PT corruption) causes RAS errors only when RAM starts at 0x80000000, not at 0x80032000.

3. **Corruption pattern points to stale data** - The garbage value 0x10011ea5 (pointing to addr 0x10011000 below DRAM) suggests memory containing old/invalid data is being reused without proper initialization.

### Recommended Next Steps

1. **Investigate memory reuse paths** - Focus on how memory is recycled between test iterations, particularly during cap revocation.

2. **Track specific PT lifecycle** - Add logging to track when corrupted PTs are created, used, and freed.

3. **Compare corruption addresses** - Determine if the same physical pages are repeatedly corrupted or if it's different pages each time.

4. **Test with RAM at intermediate addresses** - Try RAM start at 0x80010000, 0x80020000 to find the threshold where RAS errors begin.
