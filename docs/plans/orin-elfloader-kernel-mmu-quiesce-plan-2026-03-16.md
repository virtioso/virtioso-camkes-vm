# Elfloader and Kernel MMU Quiesce Plan

Date: 2026-03-16
Status: Plan
Scope: ARM64 EL2 (Orin AGX). Applicable to all AArch64 EFI platforms.

## Problem

The seL4 boot chain has two transition points where stale MMU/TLB/cache state
from the previous stage persists into the next:

1. **UEFI → elfloader**: After ExitBootServices, UEFI's EL2 stage-1 page
   tables and TLB entries remain active while boot-services memory is
   reclaimed. The elfloader runs `load_images()`, `continue_boot()`, and
   builds new page tables — all under UEFI's stale MMU state.

2. **elfloader → seL4 kernel**: The elfloader enables its own MMU, then jumps
   to the kernel's `_start`. The kernel inherits the elfloader's TTBR, TLB
   entries, and cache state without any cleanup. It modifies SCTLR but never
   invalidates TLBs or reloads TTBRs.

On Orin AGX, both transitions cause non-deterministic RAS errors because the
Cortex-A78AE hardware walker speculatively prefetches through stale TLB entries
into freed/reused memory.

Linux avoids this by establishing a clear contract at every transition: **MMU
off, caches clean, TLBs flushed, then build fresh state from scratch.** seL4's
boot chain does not follow this pattern.

## Reference: Linux's Proven Sequence

From `arch/arm64/kernel/efi-entry.S` and `arch/arm64/kernel/head.S`:

### EFI stub → kernel entry (efi-entry.S)

```
1. dcache_clean_poc    (kernel image)        -- push to PoC
2. ic ialluis                                 -- invalidate I-cache
3. dcache_clean_poc    (transition code)      -- push own code to PoC
4. bic SCTLR.M, SCTLR.C                      -- disable MMU + D-cache
5. isb                                        -- synchronize
6. br kernel_entry                            -- jump with MMU OFF
```

### kernel primary_entry (head.S)

```
1. (MMU is OFF, D-cache is OFF)
2. __create_page_tables                       -- build fresh tables (MMU off)
3. dmb sy                                     -- ensure table writes visible
4. dcache_inval_poc    (idmap tables)         -- invalidate stale cache lines
5. dcache_inval_poc    (init tables)          -- invalidate stale cache lines
6. __cpu_setup:
     tlbi vmalle1                             -- flush ALL TLB entries
     dsb nsh                                  -- barrier
     configure MAIR, TCR                      -- fresh register state
7. __enable_mmu:
     msr ttbr0_el1, idmap                     -- load fresh TTBRs
     msr ttbr1_el1, init_pg                   -- load fresh TTBRs
     isb                                      -- synchronize
     msr sctlr_el1 (M|C|I)                   -- enable MMU + caches
     isb
     ic iallu                                 -- invalidate I-cache
     dsb nsh
     isb
```

The key principle: **every stage starts from a known-clean state.** No
inherited TLB entries, no inherited TTBR values, no stale cache lines in
page table regions.

## Current seL4 State (What's Wrong)

### UEFI → elfloader (sys_boot.c)

```
efi_exit_boot_services()
-- NO MMU disable, NO TLBI, NO cache clean --
load_images()                                 -- runs under UEFI's stale MMU
continue_boot()
  flush_dcache_range(kernel, user)            -- partial: only kernel/user images
  disable_caches_hyp()                        -- LATE: disables EL2 MMU here
  init_hyp_boot_vspace()                      -- builds new page tables
  arm_enable_hyp_mmu():
    flush_dcache()                            -- flushes own tables
    disable_mmu sctlr_el2                     -- disable (already disabled above?)
    invalidate_icache
    configure MAIR, TCR
    msr ttbr0_el2                             -- load new TTBR
    isb
    tlbi alle2is                              -- flush TLB
    dsb ish, isb
    enable_mmu sctlr_el2
    ic ialluis, dsb ish, isb
    tlbi alle2is, dsb ish, isb               -- second TLBI after MMU enable
```

Problems:
- `load_images()` runs under UEFI's stale page tables for the full image
  unpacking phase
- UEFI's stale TLB entries persist until `arm_enable_hyp_mmu()` which is
  called much later
- `disable_caches_hyp()` flushes the elfloader's own D-cache but doesn't
  invalidate UEFI's stale TLB entries
- No `dcache_inval_poc` on the new page tables after building them

### elfloader → seL4 kernel (head.S)

```
_start:
  msr daifset, #MASK                          -- disable interrupts
  mrs x4, sctlr_el2
  orr/bic SCTLR bits                          -- modify in-place (NOT fresh write)
  msr sctlr_el2, x4                          -- MMU stays enabled, inherited TTBRs
  -- NO TLBI --
  -- NO DSB/ISB after SCTLR write --
  -- NO TTBR reload --
  set up stack
  bl init_kernel
```

Problems:
- Inherits elfloader's TTBRs without reloading
- Inherits elfloader's TLB entries without flushing
- Inherits elfloader's stale D-cache lines
- No clean MAIR/TCR configuration (inherits elfloader's)
- The kernel later sets up its own page tables in `init_boot_pd` and writes
  them to TTBR during `init_cpu()`, but by then stale state has already been
  active during the entire `init_kernel()` C code execution

## Implementation Plan

### Phase 1: UEFI → elfloader quiesce

**File:** `tools/seL4/elfloader-tool/src/arch-arm/sys_boot.c`

Immediately after `efi_exit_boot_services()` returns, before any other work:

```c
#if defined(CONFIG_ARCH_AARCH64)
if (is_hyp_mode()) {
    /* Step 1: Clean elfloader's own code/data to PoC so it survives
     * MMU-off execution. */
    extern char _text[], _end[];
    flush_dcache_range((unsigned long)_text, (unsigned long)_end);

    /* Step 2: Disable EL2 MMU and D-cache.
     * After this, all memory accesses are uncached and go directly
     * to physical memory. UEFI's stale page tables are no longer
     * consulted for new translations. */
    asm volatile(
        "mrs x0, sctlr_el2\n"
        "bic x0, x0, #(1 << 0)\n"   /* Clear M (MMU) */
        "bic x0, x0, #(1 << 2)\n"   /* Clear C (D-cache) */
        "msr sctlr_el2, x0\n"
        "isb\n"
        ::: "x0", "memory"
    );

    /* Step 3: Invalidate all EL2 TLB entries.
     * With MMU off, this is safe — no speculative walks can occur
     * because SCTLR.M=0 means the hardware walker is disabled. */
    asm volatile(
        "tlbi alle2is\n"
        "dsb ish\n"
        "isb\n"
        ::: "memory"
    );
}
#endif
```

**Key insight vs. our failed attempt:** The earlier TLBI attempt crashed
because we did TLBI with MMU still ON, causing speculative re-walks. Linux's
approach is to disable MMU FIRST (stopping the walker), THEN do TLBI (cleaning
up stale entries for later). The order matters.

After this quiesce, the elfloader continues with MMU off. The
`disable_caches_hyp()` call in `continue_boot()` becomes redundant but
harmless. `load_images()` runs MMU-off, which is fine since the elfloader
was designed to work without MMU on non-EFI platforms.

**Consideration:** Some UEFI implementations may place the elfloader's code
in memory that requires UEFI's page tables to be cacheable for acceptable
performance. Running `load_images()` MMU-off may be significantly slower
because all memory accesses go uncached. If performance is a concern, an
alternative is to disable MMU only after `load_images()` completes (right
before `init_hyp_boot_vspace()`), accepting the stale-TLB risk during image
loading but eliminating it before page table construction.

### Phase 2: elfloader → kernel quiesce

**File:** `kernel/src/arch/arm/64/head.S`

Before the kernel touches any system registers or memory:

```asm
_start:
    /* Save parameters (x0-x5) */
    mov     x7, x4
    mov     x8, x5

    /* Disable interrupts */
    msr     daifset, #DAIFSET_MASK

    /* ============================================================
     * Quiesce inherited elfloader MMU/TLB/cache state.
     * The elfloader left us with its MMU enabled, its TTBRs loaded,
     * and its TLB entries active. We need a clean slate before
     * setting up the kernel's own translation state.
     * ============================================================ */

#ifdef CONFIG_ARM_HYPERVISOR_SUPPORT
    /* Step 1: Disable EL2 MMU and D-cache */
    mrs     x4, sctlr_el2
    bic     x4, x4, #(1 << 0)        /* Clear SCTLR.M (MMU) */
    bic     x4, x4, #(1 << 2)        /* Clear SCTLR.C (D-cache) */
    msr     sctlr_el2, x4
    isb

    /* Step 2: Invalidate all EL2 TLB entries */
    tlbi    alle2is
    dsb     ish
    isb

    /* Step 3: Invalidate I-cache */
    ic      ialluis
    dsb     ish
    isb
#else
    /* EL1 path */
    mrs     x4, sctlr_el1
    bic     x4, x4, #(1 << 0)
    bic     x4, x4, #(1 << 2)
    msr     sctlr_el1, x4
    isb

    tlbi    vmalle1is
    dsb     ish
    isb

    ic      ialluis
    dsb     ish
    isb
#endif

    /* ============================================================
     * Now in clean state: MMU off, TLBs empty, I-cache clean.
     * Set up the kernel's translation state from scratch.
     * ============================================================ */

    /* Configure SCTLR with kernel's desired bits */
    msr     spsel, #1
#ifdef CONFIG_ARM_HYPERVISOR_SUPPORT
    ldr     x4, =CR_BITS_SET
    msr     sctlr_el2, x4            /* Fresh write, not read-modify-write */
#else
    ldr     x4, =CR_BITS_SET
    msr     sctlr_el1, x4
#endif
    isb
```

**Critical change:** The current head.S does `mrs; orr; bic; msr` (read-modify-
write) on SCTLR, inheriting whatever bits the elfloader left. The new code does
a fresh write with only the kernel's desired bits, after first disabling
everything and flushing.

**Note on MMU-off kernel execution:** The kernel's `_start` runs briefly with
MMU off (just the assembly prologue + stack setup). The `init_kernel()` C code
needs the MMU ON for kernel virtual addresses to work. The kernel enables its
own MMU during `init_cpu()` → `setCurrentKernelVSpaceRoot()` which is called
early in `try_init_kernel()`. The assembly code between `_start` and
`init_kernel` only uses physical addresses (stack, saved registers), so MMU-off
is safe there.

**However:** `init_kernel()` is a C function that expects to run with the
kernel's MMU enabled. Currently, the elfloader's `arm_enable_hyp_mmu()` sets up
a virtual mapping that maps the kernel at its link address. If we disable the
MMU in `_start`, the kernel C code won't have valid translations.

This means the kernel's `_start` needs to **re-enable the MMU** with the
kernel's own page tables before calling `init_kernel()`. The kernel's page
tables are set up by the elfloader's `init_hyp_boot_vspace()` — those tables
map the kernel image at its virtual address. The elfloader passes control with
those tables in TTBR0_EL2.

Revised sequence for `_start`:

```asm
    /* Step 1: Save current TTBR (kernel page tables from elfloader) */
    mrs     x9, ttbr0_el2

    /* Step 2: Disable MMU + flush TLBs (as above) */
    ...

    /* Step 3: Invalidate D-cache for the kernel's page table region.
     * This ensures no stale elfloader cache lines remain in the
     * page table pages. */
    /* (The elfloader already flushed its page tables in
     * arm_enable_hyp_mmu → flush_dcache, so this may be redundant
     * but is the architecturally correct thing to do.) */

    /* Step 4: Reload TTBR with the same kernel page tables */
    msr     ttbr0_el2, x9
    isb

    /* Step 5: Configure MAIR, TCR fresh (not inherited) */
    ldr     x4, =KERNEL_MAIR_VALUE
    msr     mair_el2, x4
    ldr     x4, =KERNEL_TCR_VALUE
    msr     tcr_el2, x4
    isb

    /* Step 6: Enable MMU with clean state */
    ldr     x4, =CR_BITS_SET
    msr     sctlr_el2, x4
    isb
    ic      ialluis
    dsb     ish
    isb
```

**Complication:** The kernel's MAIR and TCR values are currently set by
`init_cpu()` in C code, not in head.S. We'd need to either:
- (a) Duplicate the MAIR/TCR configuration as constants in head.S
- (b) Accept that head.S re-enables MMU with the elfloader's MAIR/TCR
  (which are identical in practice) and let `init_cpu()` reconfigure later
- (c) Move the kernel's MMU configuration entirely into head.S

Option (b) is the pragmatic choice: the elfloader already configures
MAIR/TCR identically to what the kernel wants. The important thing is the
clean TLBI + TTBR reload + ISB sequence, not reconfiguring MAIR/TCR.

### Phase 3: Validate new page tables before use

**File:** `tools/seL4/elfloader-tool/src/arch-arm/armv/armv8-a/64/mmu-hyp.S`

Add `dcache_inval_poc` on newly created page tables, matching Linux's pattern:

In `arm_enable_hyp_mmu`, after `flush_dcache` and before `msr ttbr0_el2`:

```asm
    /* After building page tables and flushing D-cache, invalidate
     * cache lines in the page table region to ensure no stale
     * UEFI-era cache content survives. flush_dcache already cleaned
     * to PoC, but invalidation ensures coherency if any speculative
     * fill occurred between the clean and this point. */
    adrp    x8, _boot_pgd_down
    dc      ivac, x8              /* invalidate PGD line */
    /* ... repeat for all table pages ... */
    dsb     ish
```

This is defense-in-depth — `flush_dcache` already does `dc civac` (clean +
invalidate), so an additional `dc ivac` should be redundant. But it matches
Linux's pattern of explicit invalidation after table creation.

## Commit Series

```
E1: elfloader: quiesce UEFI MMU/TLB state after ExitBootServices
    - Disable EL2 MMU + D-cache immediately after ExitBootServices
    - TLBI alle2is with MMU off
    - Elfloader continues running MMU-off until arm_enable_hyp_mmu

E2: kernel: quiesce elfloader MMU/TLB state at _start
    - Disable MMU, TLBI, IC IALLU at the top of head.S
    - Reload TTBR, ISB, re-enable MMU before calling init_kernel
    - Fresh SCTLR write instead of read-modify-write

E3: elfloader: invalidate D-cache on new page tables before MMU enable
    - dcache_inval after page table creation in arm_enable_hyp_mmu
    - Defense-in-depth, matches Linux pattern
```

## Risks

1. **Performance.** Running `load_images()` with MMU off means all memory
   accesses during image unpacking are uncached. On a 3MB sel4test image this
   may add noticeable boot time. Mitigation: could delay MMU disable until
   after `load_images()` (accept stale-TLB risk during loading, eliminate it
   before page table construction). This is a policy choice.

2. **UART output.** The elfloader's UART driver may depend on device memory
   mappings from UEFI's page tables. With MMU off, UART accesses go directly
   to physical addresses, which should work for MMIO (it's always uncached on
   ARM64 device memory), but the driver needs to use physical addresses, not
   virtual ones. The elfloader's UART is already configured for physical
   access in the EFI path, so this should be fine.

3. **Stack and data.** The elfloader's stack and global data are in physical
   memory. With MMU off, these are accessed by physical address. The EFI
   loader placed the elfloader in DRAM, so physical access works. But if UEFI
   placed the elfloader at a virtual address that differs from physical, we'd
   need an identity-mapped trampoline. On Orin, UEFI's EL2 stage-1 mapping is
   typically identity (VA = PA), so this should be safe.

4. **Elfloader relocation.** `relocate_below_kernel()` uses `memmove()` to
   relocate the elfloader. With MMU off, this moves physical memory. The
   relocation logic already handles physical addresses, so this should work.

5. **Kernel head.S MAIR/TCR.** If the elfloader and kernel have different
   MAIR/TCR settings, option (b) (inherit elfloader's) may cause subtle
   memory attribute issues during early kernel init. In practice, both use
   identical MAIR/TCR for normal WBWA + device nGnRnE, so this is not a
   real risk on Orin.

## Expected Outcome

After implementing E1 + E2:
- Zero RAS errors during the UEFI→elfloader transition (stale UEFI TLBs
  flushed immediately with MMU off, safe from speculative re-walks)
- Zero RAS errors during the elfloader→kernel transition (stale elfloader
  TLBs flushed, kernel starts with clean translation state)
- Combined with the frame cap slot identity + PT parent slot identity
  kernel fixes, this should eliminate ALL Orin RAS errors across the
  entire boot-to-test-completion lifecycle

## Relationship To Kernel Fixes

The elfloader/kernel quiesce work is orthogonal to the kernel vspace
redesign:
- The kernel fixes (frame cap slot identity, PT parent slot identity)
  eliminate RAS errors during **runtime** vspace operations
- The quiesce work eliminates RAS errors during **boot transitions**
- Both are needed for zero RAS errors end-to-end
