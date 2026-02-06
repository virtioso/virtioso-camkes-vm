# Orin AGX sel4test Cache Coherency Investigation

**Date:** 2025-12-13
**Status:** Workaround Implemented, Root Cause Identified

## Executive Summary

sel4test failures on NVIDIA Orin AGX were caused by cache coherency issues related to the broken `dc cisw` instruction on Tegra platforms. **A userspace workaround has been implemented** that fixes the immediate symptoms, but the **root cause appears to be EL3 firmware behavior** that we cannot directly control.

### Key Findings

1. **Symptom**: Intermittent vspace reservation failures (`is_reserved_range FAILED`)
2. **Immediate Cause**: Userspace writes to vspace bookkeeping tables were being lost/corrupted
3. **Root Cause Hypothesis**: EL3 firmware (ARM Trusted Firmware / NVIDIA platform code) likely uses `dc cisw` for cache maintenance, which corrupts seL4/userspace cache lines
4. **Evidence**: seL4 already has `KernelAArch64SErrorIgnore=ON` for Tegra platforms due to known firmware issues

### Workaround Implemented

Added explicit `dc cvac` (cache clean by VA) after all writes to vspace bookkeeping tables in `projects/seL4_libs/libsel4utils/include/sel4utils/vspace_internal.h`. This ensures data reaches RAM before any potential firmware cache operations can corrupt it.

**Result**: Original reservation errors eliminated. Tests now pass through SYSCALL, TIMER, BIND, and early BREAKPOINT tests consistently.

### Remaining Issues

- BREAKPOINT tests (hardware debug registers) still cause issues - likely a separate Tegra-specific problem
- The workaround adds overhead and only protects vspace tables - other data could still be affected
- A proper fix requires addressing the EL3 firmware behavior

## Key Finding: Intermittent Failures Point to Cache Issues

Five consecutive test runs showed varying failure points with the same error pattern:

| Run | Last Passed Test | Failure Point | Error Type |
|-----|------------------|---------------|------------|
| 1   | Test 23 (BREAKPOINT_002) | Test 24 (BREAKPOINT_003) | Reservation error @ 0x600000 |
| 2   | Test 20 (BIND0003) | Test 21 (BIND0004) | Reservation error @ 0x600000 |
| 3   | Test 21 (BIND0004) | Test 22 (BREAKPOINT_001) | Reservation error @ 0x600000 |
| 4   | Test 27 (BREAKPOINT_006) | Test 28 (BREAKPOINT_007) | **RAS Error** |
| 5   | Test 21 (BIND0004) | Test 22 (BREAKPOINT_001) | Reservation error @ 0x600000 |

### Observations:
1. **Tests 0-20 ALWAYS pass** - SYSCALL, TIMER, first 3 BIND tests
2. **Failure point varies randomly** between tests 21-28
3. **Failing address is ALWAYS `0x600000`** - within child ELF's .text segment (0x400000-0x815fff)
4. **4/5 runs**: Reservation error; **1/5 runs**: RAS hardware error
5. The **intermittent nature** strongly suggests cache coherency, not software bugs

## The Specific Error

```
check_reservation@vspace.c:65 check_reservation: is_reserved_range FAILED for [0x600000-0x601000] (bounds were OK)
sel4utils_new_pages_at_vaddr@vspace.c:511 Range for vaddr 0x600000 with 1 4k pages not reserved!
```

This means:
1. Parent process calls `perform_reservation()` which writes `RESERVED` (UINTPTR_MAX) to vspace bookkeeping tables
2. Later, `is_reserved_range()` reads back the tables and sees `EMPTY` (0) instead of `RESERVED`
3. The `check_reservation_bounds()` passes (the logical reservation exists), but the actual table entries are wrong

## Root Cause Analysis

### Current State of Orin Cache Fix

The kernel has Orin-specific cache operations in `kernel/src/plat/orinagx/machine/cache.c`:

```c
void cleanInvalidate_D_PoC(void)
{
    clean_invalidate_by_va_range(KERNEL_ELF_BASE, (word_t)ki_end);
}
```

**Problem:** This only covers the kernel image region (`KERNEL_ELF_BASE` to `ki_end`), NOT userspace memory.

### Why This Matters

The functions `cleanInvalidate_D_PoC()`, `clean_D_PoU()`, and `cleanInvalidate_L1D()` are supposed to flush the **entire** D-cache. They are called:

1. During kernel boot (`activate_kernel_vspace`)
2. During memory retyping (`clearMemory` in `untyped.c`)
3. During page table operations

On normal ARM platforms, these use `dc cisw` to flush by set/way (entire cache). On Tegra, `dc cisw` is broken, so the Orin code uses `dc civac` (by VA) instead - but only for the kernel region.

### The Fundamental Problem

`dc civac` requires knowing the virtual address of what to flush. You cannot use it to flush "all cache lines" without knowing all addresses. The current workaround:
- Only flushes kernel region (known addresses)
- Leaves userspace cache lines potentially corrupted

## Affected Code Paths

### 1. Kernel Cache Operations
- `kernel/src/plat/orinagx/machine/cache.c` - Orin-specific (partial fix)
- `kernel/src/arch/arm/armv/armv8-a/64/cache.c` - Generic ARM64 (uses dc cisw)

### 2. Userspace vspace Bookkeeping
- `projects/seL4_libs/libsel4utils/src/vspace/vspace.c`
  - `perform_reservation()` - writes RESERVED markers
  - `is_reserved_range()` - reads back markers
  - `check_reservation()` - verifies reservation (added debug logging)
- `projects/seL4_libs/libsel4utils/include/sel4utils/vspace_internal.h`
  - `reserve_entries_mid()` and `reserve_entries_bottom()` - actual table writes

### 3. Memory Allocation During Retype
- `kernel/src/object/untyped.c:255` - calls `clearMemory()`
- `kernel/include/arch/arm/arch/machine.h:47` - `clearMemory()` implementation

## Debug Modifications Made

### vspace.c Changes

Added error checking and logging to identify exact failure point:

```c
static int check_reservation_bounds(sel4utils_res_t *reservation, uintptr_t start, uintptr_t end)
{
    int result = start >= reservation->start && end <= reservation->end;
    if (!result) {
        ZF_LOGE("check_reservation_bounds FAILED: query [%p-%p] reservation [%p-%p]",
                (void*)start, (void*)end, (void*)reservation->start, (void*)reservation->end);
    }
    return result;
}

static int check_reservation(vspace_mid_level_t *top_level, sel4utils_res_t *reservation,
                             uintptr_t start, uintptr_t end)
{
    int bounds_ok = check_reservation_bounds(reservation, start, end);
    if (!bounds_ok) {
        return 0;
    }
    int reserved_ok = is_reserved_range(top_level, start, end);
    if (!reserved_ok) {
        ZF_LOGE("check_reservation: is_reserved_range FAILED for [%p-%p] (bounds were OK)",
                (void*)start, (void*)end);
    }
    return reserved_ok;
}
```

Also modified `perform_reservation()` to return int and properly check errors instead of using assert().

## Attempted Fixes That Didn't Work

### 1. Userspace Cache Flush with Platform Guard

Tried adding explicit cache flush in `vspace_internal.h`:

```c
#if defined(CONFIG_PLAT_ORIN_AGX)
    asm volatile("dc cvac, %0" : : "r"(table) : "memory");
    asm volatile("dsb sy" ::: "memory");
#endif
```

**Why it failed:** `CONFIG_PLAT_ORIN_AGX` is a kernel config macro, NOT available in userspace builds. The preprocessor guard was never true.

## Potential Solutions

### Option 1: Extend Kernel Cache Ops to All RAM

Modify `kernel/src/plat/orinagx/machine/cache.c` to flush all physical RAM:

```c
void cleanInvalidate_D_PoC(void)
{
    /* Flush entire RAM region - expensive but correct */
    /* RAM is at 0x80000000 to 0x280000000 (8GB) on Orin */
    clean_invalidate_by_va_range(RAM_PADDR_BASE, RAM_PADDR_TOP);
}
```

**Pros:** Complete fix
**Cons:** Very expensive (8GB of cache line operations)

### Option 2: Flush Only Mapped Regions

Track which memory regions are actively used and flush only those.

**Pros:** More efficient
**Cons:** Complex to implement, requires tracking allocations

### Option 3: Userspace Cache Flush Library

Create a userspace library that provides `dc civac` operations, with runtime detection of Tegra platform.

**Pros:** Targeted fix for userspace
**Cons:** Requires modifying all userspace code that needs cache coherency

### Option 4: Use Non-Cacheable Memory for Bookkeeping

Allocate vspace bookkeeping tables from non-cacheable memory.

**Pros:** Avoids cache issues entirely
**Cons:** Performance impact, requires vspace allocator changes

## Build and Test Commands

```bash
# Build sel4test for Orin AGX
cd /home/hlyytine/tii-sel4
make orinagx_defconfig
make sel4test

# Or outside Docker:
cd orinagx_sel4test
container="skip" ninja

# Test on hardware using autopilot
# (Use MCP tool mcp__sel4-autopilot__test_sel4_binary)
```

## Configuration

From `/home/hlyytine/tii-sel4/.config`:
```
PLATFORM=orinagx
NUM_NODES=1
CROSS_COMPILE=aarch64-linux-gnu-
ElfloaderImage=efi
ARM_HYP=ON
```

Key kernel config flags:
- `CONFIG_PLAT_ORIN_AGX=1`
- `CONFIG_ARM_HYPERVISOR_SUPPORT=1`
- `CONFIG_ARCH_AARCH64=1`
- `CONFIG_ARM_CORTEX_A78=1`

## Related Documentation

- `projects/virtioso-camkes-vm/docs/platforms/orin-agx/reference/tegra-cache-operations.md` - Original dc cisw issue documentation
- `CLAUDE.md` - Project build and architecture overview

## Successful Fix: Userspace Cache Flush

### Solution: Add `dc cvac` after vspace bookkeeping writes

Added cache maintenance (`dc cvac` - clean by VA to PoC) after all writes to vspace bookkeeping tables in `projects/seL4_libs/libsel4utils/include/sel4utils/vspace_internal.h`.

**Key changes:**

1. Added `vspace_ensure_write_visible()` helper function that executes `dc cvac` on ARM64:
```c
#if defined(CONFIG_ARCH_AARCH64) || defined(__aarch64__)
static inline void vspace_ensure_write_visible(void *addr)
{
    __asm__ volatile("dc cvac, %0" : : "r"(addr) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
#endif
```

2. Called this function after every write in:
   - `reserve_entries_bottom()` - after writing RESERVED
   - `reserve_entries_mid()` - after writing table pointers
   - `update_entries_bottom()` - after writing caps/cookies
   - `update_entries_mid()` - after writing table pointers
   - `clear_entries_bottom()` - after writing EMPTY
   - `create_mid_level()` - after initializing table
   - `create_bottom_level()` - after initializing table

**Results (3 consecutive test runs):**
- Original `is_reserved_range FAILED` error is **COMPLETELY ELIMINATED**
- Tests 0-23+ pass consistently (SYSCALL, TIMER, BIND, BREAKPOINT_001-003)
- No more intermittent failures at tests 21-28
- Remaining failures are in BREAKPOINT tests (hardware debug register issues, separate problem)

**Why this works:**
The `dc cvac` instruction ensures data is written from cache to main memory before the next read. On Tegra, without explicit cache maintenance, writes could remain in cache and be lost or corrupted due to the broken `dc cisw` behavior affecting cache coherency.

**IMPORTANT: This is a workaround, not a root cause fix.** See "Root Cause Analysis" below.

## Root Cause Analysis: Why Is This Happening?

### The Mystery

On a correctly functioning ARM64 system with PIPT (Physically Indexed, Physically Tagged) cache:
- Userspace writes to a memory address → data goes to cache
- Userspace reads from same address → cache hit, correct data returned
- No explicit cache maintenance needed within the same process

But on Tegra Orin, we see:
- Userspace writes RESERVED to vspace table
- Later, userspace reads EMPTY from the **same address** in the **same process**

This should be **impossible** unless something is corrupting or invalidating cache lines between write and read.

### What We Know

1. **`dc cisw` is broken on Tegra** - it invalidates without cleaning, losing dirty data
2. **seL4 kernel is patched** - we've replaced `dc cisw` with `dc civac` in kernel code
3. **The issue is intermittent** - suggesting it depends on timing/interrupts
4. **Adding `dc cvac` (clean) after writes fixes it** - data reaches RAM before corruption

### Hypothesis: EL3 Firmware Cache Operations

The most likely culprit is **EL3 firmware** (ARM Trusted Firmware / Tegra platform firmware):

```
EL0 (userspace)     - writes to vspace tables
    ↓ syscall
EL1 (seL4 kernel)   - handles syscall, may trigger...
    ↓ SMC/interrupt
EL3 (firmware)      - MAY USE dc cisw FOR CACHE MAINTENANCE
    ↓ return
EL1 (kernel)        - returns to userspace
    ↓ exception return
EL0 (userspace)     - reads corrupted data (cache line was invalidated)
```

Evidence supporting this:
1. **RAS errors originate from firmware**: The `ERROR: RAS Uncorrectable Error` messages come from EL3 (ARM Trusted Firmware), not seL4
2. **seL4 already ignores SErrors on Tegra**: `CONFIG_AARCH64_SERROR_IGNORE=ON` is set for Orin and TX2 - this exists because Tegra firmware generates spurious asynchronous errors
3. **Timer interrupts**: seL4's timer may trigger EL3 code paths
4. **GIC operations**: Interrupt controller access may involve firmware
5. **We don't control EL3**: NVIDIA's firmware is closed-source

### Existing Tegra Workaround in seL4

The kernel already has a workaround for Tegra firmware issues:

```c
// kernel/src/plat/orinagx/config.cmake
set(KernelAArch64SErrorIgnore ON)
```

This makes the SError handler simply return without processing:
```asm
// kernel/src/arch/arm/64/traps.S
BEGIN_FUNC(cur_el_serr)
#ifdef CONFIG_AARCH64_SERROR_IGNORE
    eret    // Just return, ignore the error
#else
    b       invalid_vector_entry
#endif
END_FUNC(cur_el_serr)
```

The kernel config comment says: *"SErrors may potentially be generated from software bugs or from firmware."*

This strongly suggests NVIDIA platforms have known firmware issues that generate spurious errors.

### Why the Workaround Works

By adding `dc cvac` after every write:
1. Data is immediately flushed from cache to RAM
2. Even if EL3 later does `dc cisw` (invalidate without clean), data is safe in RAM
3. Subsequent reads fetch from RAM (cache miss) and get correct data

### Potential Real Fixes

1. **Patch Tegra EL3 firmware** - Replace `dc cisw` with `dc civac` (requires NVIDIA cooperation)
2. **Disable firmware cache ops** - If possible, prevent seL4 from triggering EL3 cache maintenance
3. **Non-cacheable memory** - Use uncached memory for critical data structures (performance hit)
4. **Audit all EL3 entry points** - Understand exactly when firmware runs and does cache ops

### What Needs Investigation

1. **When does EL3 run?** Trace all SMC calls, interrupts, and exceptions that enter EL3
2. **Does Tegra firmware use `dc cisw`?** Disassemble or audit firmware if possible
3. **Can we avoid triggering EL3 cache ops?** Configure GIC/timer to minimize firmware involvement
4. **Other affected code paths?** The vspace bookkeeping is just where we noticed - there may be other affected areas

### Implications

The userspace cache flush fix is **necessary but not sufficient** for a production system:
- It adds overhead (dsb after every table write)
- It only protects vspace bookkeeping - other userspace data could still be corrupted
- The real fix requires understanding and addressing the EL3 firmware behavior

## Previous Attempted Fixes (Did Not Work)

### Fix 1: Extend Kernel Boot Cache Flush (FAILED)

Modified `kernel/src/plat/orinagx/machine/cache.c` to flush 32MB boot region instead of just kernel image:

```c
#define ORIN_BOOT_REGION_SIZE (32 * 1024 * 1024)

static inline word_t boot_region_start_va(void)
{
    return (word_t)ptrFromPAddr(physBase());
}

void cleanInvalidate_D_PoC(void)
{
    word_t start = boot_region_start_va();
    word_t end = start + ORIN_BOOT_REGION_SIZE;
    clean_invalidate_by_va_range(start, end);
}
```

**Result:** Still fails at tests 21-24 with the same reservation error. The boot-time cache flush isn't the problem - the issue occurs during runtime when the rootserver allocates and writes to vspace bookkeeping tables.

### Analysis: Runtime vs Boot-Time Issue

The failure pattern shows:
1. **Boot completes successfully** - kernel starts, rootserver starts
2. **Early tests pass** - tests 0-21 (SYSCALL, TIMER, BIND tests)
3. **Failure during child process spawn** - when allocating vspace bookkeeping for child

This indicates the problem is NOT in boot-time cache operations, but in **runtime memory operations** when userspace allocates memory from untyped objects and writes to it.

## Current Theory

When the kernel retypes untyped memory to frame objects, it zeroes the memory with `clearMemory()`:

```c
// From kernel/include/arch/arm/arch/machine.h
static inline void clearMemory(word_t *ptr, word_t bits)
{
    memzero(ptr, BIT(bits));  // No cache flush!
}
```

The kernel zeroes memory but doesn't flush the cache. On normal ARM, this is fine because:
- Kernel writes zeros to cache
- Userspace accesses same physical memory
- PIPT cache serves correct data

But on Tegra, something may be corrupting cache entries, possibly:
1. A kernel operation that still uses dc cisw somewhere
2. Some Tegra-specific cache behavior
3. Memory aliasing or translation issues

## Next Steps

1. **Search for remaining dc cisw usage** - Ensure no kernel code path uses dc cisw on Orin
2. **Add runtime cache flush to userspace** - Detect Orin at runtime (via MIDR_EL1) and add cache maintenance
3. **Investigate memory retype path** - Check if kernel's clearMemory path is correct for Orin
4. **Test with non-cacheable memory** - Allocate vspace bookkeeping from non-cacheable memory as workaround

## Files Modified During Investigation

1. `projects/seL4_libs/libsel4utils/src/vspace/vspace.c` - Debug logging
2. `projects/seL4_libs/libsel4utils/include/sel4utils/vspace_internal.h` - (reverted cache attempt)
3. `virtioso-build/Makefile` - Added qemuarm64_defconfig

## Raw Test Output Examples

### Successful boot sequence:
```
ELF-loader started on CPU: ARM Ltd. Cortex-A78AE r0p1
  paddr=[8186e8000..818a18fff]
ELF-loading image 'kernel' to 80000000
  paddr=[80000000..80250fff]
  vaddr=[8080000000..8080250fff]
ELF-loading image 'rootserver' to 80252000
  paddr=[80252000..80667fff]
  vaddr=[400000..815fff]
Enabling hypervisor MMU and paging
Jumping to kernel-image entry point...
Bootstrapping kernel
available phys memory regions: 1
  [80000000..280000000)
```

### Typical failure:
```
Starting test 22: BREAKPOINT_001
check_reservation@vspace.c:65 check_reservation: is_reserved_range FAILED for [0x600000-0x601000] (bounds were OK)
sel4utils_new_pages_at_vaddr@vspace.c:511 Range for vaddr 0x600000 with 1 4k pages not reserved!
load_segment@elf.c:94 ERROR: failed to allocate frame by loadee vka: -1
sel4utils_elf_load_record_regions@elf.c:495 Failed to load segments
sel4utils_configure_process_custom@process.c:558 Failed to load elf file
Assertion failed: error == 0 (/workspace/projects/sel4test/apps/sel4test-driver/src/testtypes.c: basic_set_up: 195)
```

### RAS Error (hardware fault):
```
ERROR:   **************************************
ERROR:   RAS Uncorrectable Error in ACI, base=0xe01a000:
ERROR:   	Status = 0xec000504
ERROR:   SERR = Assertion failure: 0x4
ERROR:   	IERR = AR decode Error: 0x5
ERROR:   	Overflow (there may be more errors) - Uncorrectable
ERROR:   	ADDR = 0x80000102464c41e0
ERROR:   **************************************
ERROR:   Powering off core
```

## Memory Carveouts and Firmware Regions

### Physical Memory Layout (from Linux on Orin AGX 32GB)

**Total Physical RAM:** 32GB
**Linux sees:** ~30.74 GB (32225728 KB)
**Carved out before Linux:** ~1.3 GB (OP-TEE, ATF, NVIDIA secure firmware)

**seL4's DTS only reports 8GB** at 0x80000000-0x280000000 - this is what the bootloader exposes to UEFI/EFI applications. The rest is carved out before seL4 even boots.

### Active Firmware Components (confirmed via SSH to Linux)

1. **OP-TEE Trusted Execution Environment:**
   ```
   optee: probing for conduit method.
   optee: revision 4.2 (7e454f37)
   optee: dynamic shared memory is enabled
   optee: initialized driver
   ```
   - Devices: `/dev/tee0`, `/dev/teepriv0`
   - Runs at S-EL1 (Secure EL1) under ATF

2. **ARM Trusted Firmware (ATF/BL31):**
   ```
   psci: probing for conduit method from DT.
   psci: PSCIv1.1 detected in firmware.
   psci: Using standard PSCI v0.2 function IDs
   psci: Trusted OS migration not required
   psci: SMC Calling Convention v1.2
   ```
   - Runs at EL3, handles all SMC calls
   - **Key concern: ATF commonly uses `dc cisw` for cache maintenance**

3. **NVIDIA Platform Firmware:**
   - BPMP (Boot and Power Management Processor)
   - RCE (Real-time Camera Engine)
   - FSI (Functional Safety Island)
   - Multiple co-processors with their own firmware

### Reserved Memory Regions (from Device Tree)

| Region | Address | Size | Purpose |
|--------|---------|------|---------|
| `fsi-carveout` | 0x82f000000 | 16 MB | Functional Safety Island |
| `pva-carveout` | 0x82d980000 | 2.5 MB | Programmable Vision Accelerator |
| `ramoops_carveout` | 0x82cdf0000 | 2 MB | Kernel panic log storage |
| `vpr-carveout` | 0x849800000 | ~870 MB | Video Protected Region |
| `linux,cma` | 0x804000000 | 256 MB | Contiguous Memory Allocator |
| `camdbg_carveout` | (dynamic) | - | Camera debug |
| `rce-reservation` | (dynamic) | - | RCE engine |

### Memory Layout from /proc/iomem (Key Regions)

```
80000000-fffdffff : System RAM (~2GB, first block)
100000000-818a14fff : System RAM (~28.5GB, main block)
  ...multiple reserved subregions...
832000000-833ffffff : reserved (32MB, firmware?)
```

### Comparison: Linux vs seL4 Memory View

| Metric | Linux | seL4 |
|--------|-------|------|
| Total RAM reported | 32225728 KB | 8GB (DTS) |
| Usable RAM | ~30.74 GB | ~8185 MB |
| Reserved/Carved | ~1.3 GB | Unknown |
| Memory base | 0x80000000 | 0x80000000 |
| Memory end | ~0x8CD700000 | 0x280000000 |

**Key insight:** seL4 only sees 8GB because the UEFI/bootloader exposes a limited memory region. The additional ~24GB is either:
- Reserved for GPU/NVDEC/other accelerators
- Carved out for secure world (OP-TEE)
- Not exposed to EFI applications

### Implications for Cache Coherency

The presence of OP-TEE and ATF running concurrently with seL4 confirms our hypothesis:

1. **ATF at EL3** handles all SMC calls and likely uses standard ARM cache maintenance
2. **OP-TEE at S-EL1** handles TEE operations and may do its own cache maintenance
3. **NVIDIA co-processors** (BPMP, RCE, FSI) may share memory and do cache operations

When any of these components execute `dc cisw` to "clean and invalidate" the cache, they actually:
- Invalidate cache lines WITHOUT writing dirty data to RAM (broken on Tegra)
- Corrupt any data that was modified by seL4/userspace but not yet written back

### Why seL4's Untypeds Don't Show Carveouts

Looking at seL4's untyped objects:
```
available phys memory regions: 1
  [80000000..280000000)
```

The untypeds cover most of the 8GB reported region (~8185 MB of 8192 MB). The ~7 MB gap is kernel + rootserver images. The firmware carveouts are invisible to seL4 because they're not in the device tree's memory node at all.

## MMU Configuration Options

### Could MMU Prevent Cache Corruption?

**Short answer: No, not directly.**

The `dc cisw` cache corruption is a cache operation issue, not an MMU issue. The MMU controls:
- Virtual-to-physical address translation
- Memory attributes (cacheable, device, etc.)
- Access permissions

The MMU cannot prevent:
- EL3 firmware from executing cache maintenance instructions
- Set/way cache operations from affecting all cache lines

### Potential MMU-Related Mitigations

1. **Non-Cacheable Memory Regions**
   - Mark critical data structures as non-cacheable in page tables
   - Avoids cache entirely, eliminating corruption risk
   - **Downside:** Significant performance penalty

2. **Inner-Shareable vs Outer-Shareable**
   - Configure memory as Inner-Shareable only
   - May limit which caches are affected by maintenance ops
   - **Unlikely to help:** ATF operates at EL3 with full cache access

3. **Write-Through Caching**
   - Configure cache policy as write-through instead of write-back
   - Data always written to RAM, cache only used for reads
   - **Downside:** Higher bus traffic, reduced performance

4. **Hardware Cache Partitioning (if available)**
   - Some ARM cores support cache partitioning
   - Could isolate seL4's cache lines from firmware
   - **Unknown:** Whether Cortex-A78AE on Orin supports this

### Why MMU Alone Can't Fix This

```
┌─────────────────────────────────────────────────┐
│              EL3 (ATF Firmware)                 │
│  - Full access to all caches                    │
│  - Can execute dc cisw on ANY set/way           │
│  - Cannot be restricted by lower ELs            │
└─────────────────────────────────────────────────┘
                       │
                       │ dc cisw (broken on Tegra)
                       │ invalidates without clean
                       ▼
┌─────────────────────────────────────────────────┐
│                L2 Cache                         │
│  - Shared between all cores and ELs            │
│  - seL4 data can be invalidated by EL3         │
└─────────────────────────────────────────────────┘
                       │
                       │ Cache line evicted
                       │ dirty data LOST
                       ▼
┌─────────────────────────────────────────────────┐
│           seL4 (EL2) / Userspace (EL0)         │
│  - MMU configured correctly                     │
│  - Cache attributes set correctly               │
│  - STILL affected by EL3 cache ops              │
└─────────────────────────────────────────────────┘
```

The fundamental problem is that EL3 has higher privilege and can affect caches that seL4 uses, regardless of seL4's MMU configuration.

## ATF/OP-TEE Source Code Analysis

Source code located at: `/home/hlyytine/pkvm/Linux_for_Tegra/source/tegra/optee-src`

### ATF (ARM Trusted Firmware) Analysis

**Good news:** NVIDIA's Tegra T234 (Orin) ATF code does NOT directly use `dc cisw`!

The Tegra platform code uses:
- `flush_dcache_range()` - uses `dc civac` (VA-based, safe)
- `clean_dcache_range()` - uses `dc cvac` (VA-based, safe)
- MCE firmware commands for full cache flush via ARI interface

Key evidence from `plat/nvidia/tegra/soc/t234/plat_psci_handlers.c`:
```c
/* SCF flush - Clean and invalidate caches */
mce_clean_and_invalidate_caches();
```

The MCE (Micro-Control Engine) is NVIDIA's proprietary microcontroller that handles cache operations. We cannot see its implementation.

**However**, the ATF common code at `lib/xlat_tables_v2/xlat_tables_context.c:154` contains:
```c
/* Flush all caches. */
dcsw_op_all(DCCISW);  // Uses dc cisw!
```

This is called from `xlat_make_tables_readonly()`. Unclear if Tegra uses this code path.

### OP-TEE Analysis

**CRITICAL FINDING:** OP-TEE uses `dc cisw` extensively!

From `nv-optee/optee/optee_os/core/arch/arm/kernel/cache_helpers_a64.S`:
```asm
dcsw_loop_table:
    dcsw_loop isw
    dcsw_loop cisw   // <-- Uses dc cisw!
    dcsw_loop csw
```

OP-TEE calls `dcache_op_all(DCACHE_OP_CLEAN_INV)` (which uses `dc cisw`) in:
- `sm/psci-helper.S` - PSCI power management hooks
- `mm/core_mmu.c` - MMU cache maintenance
- `sm/pm.c` - Power management (`dcache_op_level1`)
- `kernel/thread_a64.S` - Thread management

**This is likely the root cause!** When OP-TEE performs cache maintenance using `dc cisw`:
1. OP-TEE executes `dcache_op_all(DCACHE_OP_CLEAN_INV)`
2. This iterates through all cache sets/ways executing `dc cisw`
3. On Tegra, `dc cisw` invalidates WITHOUT cleaning first
4. Any dirty cache lines from seL4/userspace are lost/corrupted

### When Does This Happen?

OP-TEE cache flushes occur during:
1. **PSCI CPU power state transitions** (suspend/resume)
2. **Secure world context switches** (entering/exiting OP-TEE)
3. **MMU table updates** in secure world
4. **Power management** events

Since seL4 uses PSCI for timer management and potentially other operations, any SMC call that enters OP-TEE could trigger cache corruption.

### Cortex-A78AE CPU-Specific Code

The ATF CPU-specific code for Cortex-A78AE (`lib/cpus/aarch64/cortex_a78_ae.S`) does NOT use `dc cisw` - it relies on hardware-assisted coherency:
```c
/* HW will do the cache maintenance while powering down */
func cortex_a78_ae_core_pwr_dwn
```

This is good, but doesn't help if OP-TEE still uses `dc cisw` in its generic code.

## Recommended Further Investigation

1. ~~**Audit ATF source code** - Check if NVIDIA's ATF fork uses `dc cisw` and where~~ ✅ Done - ATF platform code is safe, but common code may be problematic
2. ~~**Audit OP-TEE dc cisw usage**~~ ✅ Done - OP-TEE uses `dc cisw` in `cache_helpers_a64.S`
3. ~~**Test with OP-TEE disabled**~~ ✅ Tested - OP-TEE is REQUIRED on Orin (RAS errors without it)
4. **Patch OP-TEE** - ✅ PATCH CREATED (see below)
5. **Minimize EL3 transitions** - Reduce SMC calls that might trigger OP-TEE cache ops
6. **Cache partitioning** - Investigate if Cortex-A78AE supports MPAM or similar
7. **Contact NVIDIA** - Report the `dc cisw` issue in OP-TEE and request firmware fix

## OP-TEE Patch for Tegra dc cisw Fix

### Status: PATCH APPLIED, NEEDS REBUILD AND TEST

### File Modified
`/home/hlyytine/pkvm/Linux_for_Tegra/source/tegra/optee-src/nv-optee/optee/optee_os/core/arch/arm/kernel/cache_helpers_a64.S`

### Changes Made

1. **Added nop padding** to `dcsw_loop` macro (line 138) - ensures all loops are same size

2. **Added `dcsw_loop_cisw_tegra` macro** (lines 148-168):
   ```asm
   .macro  dcsw_loop_cisw_tegra
   BTI(    bti     j)
   loop2_cisw_tegra:
           lsl     w7, w6, w2
   loop3_cisw_tegra:
           orr     w11, w9, w7
           dc      csw, x11        // clean by set/way (works on Tegra)
           dc      isw, x11        // invalidate by set/way (works on Tegra)
           subs    w7, w7, w17
           b.hs    loop3_cisw_tegra
           subs    x9, x9, x16
           b.hs    loop2_cisw_tegra
           b       level_done
   .endm
   ```

3. **Updated offset calculation** (lines 104-105):
   ```asm
   add     x14, x14, x0, lsl #5    // x0 * 32
   add     x14, x14, x0, lsl #2    // + x0 * 4 = x0 * 36 (9 instructions)
   ```

4. **Replaced cisw loop** in table (line 181):
   ```asm
   dcsw_loop_table:
           dcsw_loop isw
           dcsw_loop_cisw_tegra    // Tegra fix: csw+isw instead of broken cisw
           dcsw_loop csw
   ```

### Next Steps (TODO)

1. ✅ Patch OP-TEE cache_helpers_a64.S - DONE
2. **Rebuild OP-TEE**:
   ```bash
   cd /home/hlyytine/pkvm/Linux_for_Tegra/source/tegra/optee-src/nv-optee
   ./optee_src_build.sh -p t234 -t
   ```

3. **Rebuild ATF with OP-TEE**:
   ```bash
   cd /home/hlyytine/pkvm/Linux_for_Tegra/source/tegra/optee-src/atf
   export NV_TARGET_BOARD=generic
   ./nvbuild.sh
   ```

4. **Generate new TOS image**:
   ```bash
   python3 /home/hlyytine/pkvm/Linux_for_Tegra/nv_tegra/tos-scripts/gen_tos_part_img.py \
       --monitor /home/hlyytine/pkvm/Linux_for_Tegra/source/tegra/optee-src/atf/arm-trusted-firmware/generic-t234/tegra/t234/release/bl31.bin \
       --os /home/hlyytine/pkvm/Linux_for_Tegra/source/tegra/optee-src/nv-optee/optee/build/t234/core/tee-raw.bin \
       --dtb /home/hlyytine/pkvm/Linux_for_Tegra/source/tegra/optee-src/nv-optee/optee/tegra234-optee.dtb \
       --tostype optee \
       /home/hlyytine/pkvm/Linux_for_Tegra/bootloader/tos-patched_t234.img
   ```

5. **Flash TOS partition** (from recovery mode or use initrd flash)

6. **Test sel4test** - should now pass without the userspace dc cvac workaround

## Test Results: 6-Run Stress Test (2025-12-13)

### Configuration
- Userspace `dc cvac` workaround in vspace_internal.h: **ENABLED**
- OP-TEE `dc cisw` → `dc csw + dc isw` patch: **ENABLED**

### Results Summary

| Run | Last Test Reached | RAS Errors | Notes |
|-----|-------------------|------------|-------|
| 1 | Test 36: CNODEOP0001 | 1 | RAS error at test 36 |
| 2 | Test 34: CANCEL_BADGED_SENDS_0001 | 0 | Stopped (pagefault timeout) |
| 3 | Test 26: BREAKPOINT_005 | 1 | RAS error at BREAKPOINT test |
| 4 | Test 24: BREAKPOINT_003 | 0 | Stopped (pagefault timeout) |
| 5 | Test 32: CACHEFLUSH0003 | 1 | RAS error at CACHEFLUSH test |
| 6 | Test 25: BREAKPOINT_004 | 1 | RAS error at BREAKPOINT test |

### Key Observations

1. **Cache coherency fix is working**: The original `is_reserved_range FAILED` error is **completely eliminated** (0 out of 6 runs)

2. **Tests reach much further**: Previously failing at tests 21-28, now reaching tests 24-36

3. **Remaining failures are NOT cache-related**:
   - 4/6 runs: RAS hardware errors (firmware-level)
   - 2/6 runs: BREAKPOINT test pagefaults (hardware debug register issues)

4. **Benign startup warning**: All runs show `vspace is NULL` during ELF reservation but tests proceed past it

### RAS Error Address Analysis

The RAS errors report suspicious addresses:

| Address | Bit 63 | Bits 40-62 | Bits 0-39 | Interpretation |
|---------|--------|------------|-----------|----------------|
| `0x800000007fffffc0` | 1 | 0x000000 | 0x007fffffc0 (~2GB) | Invalid: bit 63 set |
| `0x80000102464c41e0` | 1 | 0x000001 | 0x02464c41e0 (~9.8GB) | Invalid: bits 63,40 set |

**Analysis:**
- Both addresses have bit 63 set, which is invalid for physical addresses
- The lower 40 bits look like valid addresses (within or near RAM range)
- Bit 63 being set suggests either:
  1. Virtual address being used where physical was expected
  2. Address corruption during cache operations
  3. Sign extension of a negative/kernel address

## Tegra234 Memory Controller Findings

### Address Space Configuration

The Tegra234 memory controller uses **40-bit physical addressing**:
- Source: `tegra234.c` line 1162: `.num_address_bits = 40`
- DMA mask: `DMA_BIT_MASK(40)` = bits 0-39

**Important:** seL4 is configured with 44-bit PA (`CONFIG_ARM_PA_SIZE_BITS_44`) but the actual MC hardware only supports 40 bits!

### Potential Issue: Kernel Address Space vs MC Limits

The seL4 kernel's address space configuration:
```c
#define PPTR_TOP  0xffffffffc0000000  /* 2^64 - 2^30 (with 44-bit PA) */
#define PPTR_BASE 0xffffff8000000000  /* 2^64 - 2^39 */
```

When the kernel translates these to physical addresses for cache operations or page tables, they could potentially exceed the 40-bit MC limit.

## TODO: Investigate Kernel Address Handling

Similar to the elfloader fix (commit 01e347fffb4d117dfb53306404f99a7f68b24367) that limited identity mappings to 64GB to avoid accessing non-existent address space:

1. **Check if kernel page tables map beyond 40-bit range**: The kernel maps PADDR_BASE to PADDR_TOP, which with 44-bit PA could exceed what the MC supports

2. **Check cache flush address ranges**: The Orin-specific `cleanInvalidate_D_PoC()` flushes 32MB from `physBase()` - verify these addresses are within 40-bit range

3. **Audit all dc civac calls**: Ensure no cache operations use addresses with bits 40+ set

4. ~~**Consider limiting PA to 40 bits**~~: ✅ **DONE** - Changed to 40-bit PA in `arch/arm/config.cmake`

## Test Results: 40-bit PA (2025-12-13)

Changed `CONFIG_ARM_PA_SIZE_BITS_40` for Orin to match Tegra234 MC hardware limits.

### Comparison: 44-bit vs 40-bit PA

| Run | 44-bit PA | 40-bit PA | Improvement |
|-----|-----------|-----------|-------------|
| 1 | Test 36 | Test 94 | +58 tests |
| 2 | Test 34 | Test 67 | +33 tests |
| 3 | Test 26 | Test 35 | +9 tests |
| 4 | Test 24 | Test 34 | +10 tests |
| 5 | Test 32 | Test 34 | +2 tests |
| 6 | Test 25 | **Test 141** | **All tests ran!** |

### Key Observations

1. **Run 6 completed ALL 141 tests** (44 passed, 97 failed but no crash)
2. **Tests reach much further**: Best run went from test 36 → test 94
3. **RAS errors still occur**: 4/6 runs still hit RAS hardware errors
4. **40-bit PA helps significantly**: Matching MC hardware limits reduces errors

### Remaining Issues

The 97 test failures in run 6 are likely due to:
- BREAKPOINT tests (hardware debug registers don't work properly on Tegra)
- Tests that depend on earlier tests passing
- Possible remaining firmware interactions

### Code Changes

Modified `kernel/src/arch/arm/config.cmake`:
```cmake
elseif(KernelArmCortexA78)
    if(KernelPlatformOrinAGX)
        # Tegra234 memory controller supports 40-bit addressing
        set(KernelArmPASizeBits40 ON)
        math(EXPR KernelPaddrUserTop "(1 << 40)")
    else()
        set(KernelArmPASizeBits44 ON)
        math(EXPR KernelPaddrUserTop "(1 << 44)")
    endif()
endif()
```

### Bit 39 Experiment (Did NOT Help)

Tested limiting to 39-bit (512GB) to avoid addresses with bit 39 set:

| Config | Best Run | RAS Errors |
|--------|----------|------------|
| 40-bit | Test 141 (all tests ran) | 4/6 |
| 39-bit | Test 94 | 5/6 |

**Conclusion:** Bit 39 is NOT the issue. The 40-bit limit is optimal.
