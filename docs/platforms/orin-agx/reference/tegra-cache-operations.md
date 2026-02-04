# Tegra Cache Operations: dc civac vs dc cisw

## Summary

On NVIDIA Tegra Xavier and Orin SoCs, the ARM `dc cisw` (clean and invalidate by set/way) instruction does not work correctly. Use `dc civac` (clean and invalidate by virtual address to Point of Coherency) instead.

## Problem

The `dc cisw` instruction is commonly used to flush the entire data cache before enabling/disabling the MMU or transferring control between execution contexts. However, on Tegra Xavier/Orin platforms, this instruction **invalidates cache lines without properly writing data to DRAM first**, resulting in data loss.

### Symptoms

- Page table entries appear corrupted after cache flush
- Memory contents read back as garbage/uninitialized values after flush
- Random crashes after MMU enable with correct page table setup
- Stack corruption visible when reading the same address before and after flush

### Root Cause

The `dc cisw` instruction operates on the local CPU's view of the cache using set/way addressing. On complex SoCs like Tegra with:
- Multiple cache levels
- System-level caches
- Cache coherency interconnects

The set/way operation may not properly propagate to all levels, or the cache hierarchy may have implementation-specific behavior that causes premature invalidation.

## Solution

Replace `dc cisw` with `dc civac` (clean and invalidate by virtual address to PoC). This instruction:
- Operates on a specific virtual address
- Guarantees data is written to the Point of Coherency (typically DRAM)
- Works correctly across all cache levels

### Code Example

**Before (broken on Tegra):**
```asm
/* Flush entire D-cache using set/way - BROKEN on Tegra */
.macro dcache op
    dsb     sy
    mrs     x0, clidr_el1
    /* ... iterate through cache levels ... */
    dc      \op, x11      /* cisw = clean and invalidate by set/way */
    /* ... */
    dsb     sy
    isb
.endm

flush_dcache:
    dcache  cisw
    ret
```

**After (works on Tegra):**
```asm
/* Flush D-cache by VA range - works on Tegra */
flush_dcache:
    /* Flush from start_addr to end_addr */
    adrp    x0, _start_symbol
    adrp    x1, _end_symbol
    add     x1, x1, #0x1000     /* round up */

    /* Get cache line size from CTR_EL0 */
    mrs     x2, ctr_el0
    ubfx    x2, x2, #16, #4     /* DminLine field */
    mov     x3, #4
    lsl     x3, x3, x2          /* cache line size in bytes */

1:  dc      civac, x0           /* clean and invalidate by VA to PoC */
    add     x0, x0, x3
    cmp     x0, x1
    b.lo    1b

    dsb     sy
    isb
    ret
```

## Affected Components

The following seL4/elfloader components use `dc cisw` and need fixes for Tegra:

### 1. elfloader-tool - `src/arch-arm/armv/armv8-a/64/mmu.S` ✅ FIXED
- `flush_dcache` function - now uses `dc civac`
- `flush_kernel_image` function - flushes kernel image region (0x80000000-0x80800000) after MMU disable

**Important:** The kernel image must also be flushed from the cache. The kernel is loaded via `memcpy` to physical address 0x80000000 while UEFI's MMU is active. This data may remain in the cache. The flush must be done with MMU disabled (using physical addresses) before enabling our own page tables and jumping to the kernel.

### 2. seL4 kernel - `kernel/src/arch/arm/armv/armv8-a/64/cache.c` ✅ FIXED
- Generic implementation is now conditional: `#if !defined(CONFIG_PLAT_ORIN_AGX)`
- Platform-specific implementation in `kernel/src/plat/orinagx/machine/cache.c`

**Platform cache.c functions using `dc civac`:**
- `clean_D_PoU()` - cleans kernel image range
- `cleanInvalidate_D_PoC()` - cleans and invalidates kernel image range
- `cleanInvalidate_L1D()` - delegates to `cleanInvalidate_D_PoC()`

### 3. Platform-specific code - Boot sequences, SMP bringup
- May have additional cache operations that need auditing

## Testing

To verify cache flush correctness:

1. Write known values to memory
2. Call cache flush
3. Read values back immediately after flush
4. Values should match - if garbage, flush is broken

Example debug output showing the problem:
```
Before flush: pgd0=0000000818A12003  (correct)
After flush:  pgd0=BE4CDE00A8428275  (garbage - data lost!)
```

## Implementation Plan for seL4 Kernel Fix

The seL4 kernel needs platform-specific cache implementations for Tegra. Here's the recommended approach:

### Option A: Platform-specific cache.c (Recommended)

Create a Tegra-specific implementation that overrides the generic ARMv8-A cache functions.

**Step 1: Create platform cache implementation**

Create `kernel/src/plat/orinagx/machine/cache.c`:

```c
/*
 * Tegra-specific cache operations using dc civac instead of dc cisw.
 * The dc cisw instruction does not work correctly on Tegra Xavier/Orin.
 */

#include <arch/machine/hardware.h>
#include <plat/machine/hardware.h>

/* Clean and invalidate by VA range */
static void cleanInvalidate_by_va(word_t start, word_t end)
{
    word_t line_size = getCacheLineSizeBits();
    word_t line = BIT(line_size);

    start = ROUND_DOWN(start, line);
    end = ROUND_UP(end, line);

    for (word_t addr = start; addr < end; addr += line) {
        asm volatile("dc civac, %0" : : "r"(addr) : "memory");
    }
    dsb();
}

/* Clean by VA range */
static void clean_by_va(word_t start, word_t end)
{
    word_t line_size = getCacheLineSizeBits();
    word_t line = BIT(line_size);

    start = ROUND_DOWN(start, line);
    end = ROUND_UP(end, line);

    for (word_t addr = start; addr < end; addr += line) {
        asm volatile("dc cvac, %0" : : "r"(addr) : "memory");
    }
    dsb();
}

/*
 * For whole-cache operations, we need to know what memory ranges to flush.
 * Options:
 * 1. Flush known kernel memory regions (kernel image + page tables)
 * 2. Use platform-specific memory map
 * 3. Skip if not strictly necessary (many uses are conservative)
 */

void clean_D_PoU(void)
{
    /* Clean kernel text/rodata for instruction fetch coherency */
    extern char _start[], _end[];
    clean_by_va((word_t)_start, (word_t)_end);
}

void cleanInvalidate_D_PoC(void)
{
    /* Clean and invalidate kernel memory */
    extern char _start[], _end[];
    cleanInvalidate_by_va((word_t)_start, (word_t)_end);

    /* Also need to handle page tables - platform specific */
    /* TODO: Add page table regions */
}

void cleanInvalidate_L1D(void)
{
    /* Same as PoC for Tegra - dc civac goes to PoC */
    cleanInvalidate_D_PoC();
}
```

**Step 2: Update platform CMakeLists.txt**

In `kernel/src/plat/orinagx/config.cmake`, add the platform cache implementation:

```cmake
# Use platform-specific cache operations (dc civac instead of dc cisw)
add_sources(
    DEP "KernelPlatformOrinAGX"
    PREFIX src/plat/orinagx/machine
    CFILES cache.c
)
```

**Step 3: Exclude generic implementation for Tegra**

Modify `kernel/src/arch/arm/armv/armv8-a/64/cache.c` to be conditional:

```c
#if !defined(CONFIG_PLAT_ORINAGX) && !defined(CONFIG_PLAT_TX2)
/* ... existing generic implementation ... */
#endif
```

Or better, use weak symbols so platform can override.

### Option B: Conditional compilation in generic cache.c

Add `#ifdef CONFIG_PLAT_ORINAGX` blocks to use `dc civac` on Tegra platforms.

Less clean but simpler for a single platform.

### Option C: Runtime detection

Detect Tegra at runtime and choose implementation. More complex, probably overkill.

### Testing the Fix

1. Build sel4test for orinagx: `make sel4test`
2. Run via autopilot
3. Verify kernel boots and produces output
4. Run full sel4test suite

### Files to Modify

| File | Change |
|------|--------|
| `kernel/src/plat/orinagx/config.cmake` | Add platform cache.c |
| `kernel/src/plat/orinagx/machine/cache.c` | Create with dc civac impl |
| `kernel/src/arch/arm/armv/armv8-a/64/cache.c` | Make generic impl conditional or weak |
| `kernel/include/plat/orinagx/plat/machine/hardware.h` | Add any needed declarations |

## References

- ARM Architecture Reference Manual - Cache maintenance instructions
- NVIDIA Tegra Technical Reference Manual
- seL4 elfloader commit fixing this issue (dc civac implementation)

## Platform Applicability

| Platform | dc cisw | dc civac |
|----------|---------|----------|
| Tegra Xavier (T194) | BROKEN | Works |
| Tegra Orin (T234) | BROKEN | Works |
| Raspberry Pi 4 (BCM2711) | Works | Works |
| QEMU virt | Works | Works |

Always prefer `dc civac` for maximum portability on ARM platforms.
