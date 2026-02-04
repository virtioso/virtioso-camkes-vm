# ARM64 Whole-Cache Operations: Why dc cisw Is Broken

## Summary

This document explains why set/way cache maintenance operations (dc cisw, dc csw) are architecturally broken on modern ARM64 SoCs, not just NVIDIA Tegra. Linux ARM64 removed `flush_cache_all()` entirely in 2015 for this reason.

**Key Finding**: The seL4 kernel's whole-cache operations (`clean_D_PoU`, `cleanInvalidate_D_PoC`, `cleanInvalidate_L1D`) cannot be implemented correctly on ARM64. The solution is to replace them with VA-based range operations.

## Background

### The Problem with Set/Way Operations

The ARM `dc cisw` (Clean and Invalidate by Set/Way) instruction is commonly used to flush the entire data cache. However, on modern ARM64 systems, this operation has fundamental architectural limitations.

From Mark Rutland's (ARM Ltd) patch removing `flush_cache_all()` from Linux ARM64:

> "The documented semantics of flush_cache_all are **not possible to provide** for arm64 (short of flushing the entire physical address space by VA)."

### Why Set/Way Operations Are Broken (ALL ARM64, not just Tegra)

1. **Race with CPU speculation** - Background behavior (speculation, prefetch) may allocate/evict/migrate cache lines. Depending on cache topology, this may "hide" lines from subsequent set/way operations.

2. **Not broadcast** - Set/way operations don't affect other CPUs. Depending on cache coherency protocols, other CPUs may retain dirty/shared lines that won't be operated on.

3. **Don't affect system caches** - System caches (L3, LLC) only respect VA-based maintenance in ARMv8-A. Set/way operations only affect CPU-local caches.

4. **Only work with caches disabled** - These operations "do not make sense to use while SCTLR.C or SCTLR.M are set" (i.e., while caches are enabled).

### Tegra-Specific Manifestation

On NVIDIA Tegra Xavier and Orin SoCs, the `dc cisw` instruction additionally exhibits incorrect behavior: it invalidates cache lines without properly writing dirty data to DRAM first, causing data corruption. This is a more severe manifestation of the general ARM64 limitation.

## Linux's Solution

Linux ARM64 took the following approach:

1. **Removed `flush_cache_all()` entirely** - The function doesn't exist on ARM64
2. **KVM uses VA maintenance exclusively** - Even the hypervisor uses VA-based operations
3. **kexec uses VA-based maintenance** - Specific regions are flushed by VA, not "flush all"

## seL4 Whole-Cache Functions

seL4 has these whole-cache functions that are affected:

| Function | Purpose | Problem |
|----------|---------|---------|
| `clean_D_PoU()` | Clean D-cache to Point of Unification | Uses dc csw (set/way) |
| `cleanInvalidate_D_PoC()` | Clean+invalidate D-cache to Point of Coherency | Uses dc cisw (set/way) |
| `cleanInvalidate_L1D()` | Clean+invalidate L1 D-cache | Uses dc cisw (set/way) |

### Callers Analysis (64-bit ARM)

| Caller | Function Called | Context |
|--------|-----------------|---------|
| `activate_kernel_vspace()` | `cleanInvalidateL1Caches()` | BOOT_CODE |
| `try_init_kernel():575` | `cleanInvalidateL1Caches()` | BOOT_CODE |
| `try_init_kernel():612` | `cleanInvalidateL1Caches()` | BOOT_CODE |
| `arch_clean_invalidate_caches()` | both | Benchmark syscall |
| `arch_clean_invalidate_L1_caches()` | `cleanInvalidate_L1D()` | Benchmark syscall |

**Key Finding**: On 64-bit ARM, the only runtime caller is the benchmark syscall (`handle_SysBenchmarkFlushCaches`). All other calls are during boot.

## Solution: VA-Based Range Operations

### The Correct Approach

Instead of "flush entire cache", flush specific memory regions by virtual address:

```c
// Instead of: cleanInvalidate_D_PoC()
// Use: cleanInvalidateCacheRange_RAM(start, end, pstart)

// The existing VA-range functions work correctly:
// - cleanCacheRange_PoC(start, end, pstart)
// - cleanInvalidateCacheRange_RAM(start, end, pstart)
// - cleanCacheRange_PoU(start, end, pstart)
```

### Boot-Time Regions to Flush

1. **Kernel page tables**: `armKSGlobalKernelPGD` to end of kernel PT allocations
2. **Rootserver allocations**: `rootserver.paging.start` to `rootserver.paging.end`
3. **Bootinfo**: `bi_frame_vptr` region
4. **Kernel image**: `ki_boot` to `ki_end` (for I-cache coherency)

### Reference Implementation: Elfloader

The seL4 elfloader has already been fixed to use VA-based operations:

```asm
// tools/seL4/elfloader-tool/src/arch-arm/armv/armv8-a/64/mmu.S
flush_dcache_range:
1:  dc      civac, x0       // Clean and invalidate by VA
    add     x0, x0, x2
    cmp     x0, x1
    b.lo    1b
    dsb     sy
    ret
```

The elfloader flushes specific regions:
- Elfloader code (_text to _end)
- All boot page tables
- Loaded kernel image
- Loaded rootserver/user image

## Implementation for seL4 Kernel

### 1. Replace Boot-Time Whole-Cache Flushes

Modify `kernel/src/arch/arm/kernel/boot.c` to flush specific regions instead of calling `cleanInvalidateL1Caches()`.

### 2. Handle Benchmark Syscall

Return error for `SysBenchmarkFlushCaches` on ARM64:

```c
#ifdef CONFIG_ARCH_AARCH64
    userError("SysBenchmarkFlushCaches: not supported on ARM64");
    current_syscall_error.type = seL4_IllegalOperation;
    return EXCEPTION_SYSCALL_ERROR;
#endif
```

### 3. Remove Whole-Cache Functions from Platform Code

For Tegra (and potentially all ARM64), remove the whole-cache functions and provide build-time assertions to catch any remaining callers.

## References

- [Mark Rutland: arm64: kill flush_cache_all()](https://lore.kernel.org/linux-arm-kernel/20150420180255.GB17767@leverpostej/)
- [LKML: Delete flush cache all in arm64 platform](https://linux.kernel.narkive.com/YHaGxcNb/delete-flush-cache-all-in-arm64-platform)
- [Xen Bug #50 - set/way cache maintenance](https://bugs.xenproject.org/xen/bug/50)
- [ARM Architecture Reference Manual - Cache Maintenance](https://developer.arm.com/documentation/ddi0488/d/system-control/aarch64-register-summary/aarch64-cache-maintenance-operations)

## Related Documentation

- `tegra-cache-operations.md` - Tegra-specific dc civac vs dc cisw issue
- `orin-ras-error-investigation.md` - RAS errors from NULL VTTBR during TLB invalidation

## Changelog

| Date | Change |
|------|--------|
| 2025-12-14 | Initial documentation - ARM64 whole-cache operations investigation |
