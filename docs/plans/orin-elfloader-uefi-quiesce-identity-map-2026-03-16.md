# Elfloader UEFI Quiesce: Temp Identity Map Implementation

Date: 2026-03-16
Status: In progress
Scope: `tools/seL4/elfloader-tool` (elfloader-tool repo)
Platform: Orin AGX (applicable to all AArch64 EFI platforms)

## Background

Continues from:
- `orin-elfloader-kernel-mmu-quiesce-plan-2026-03-16.md` (Phase 1 plan)
- `orin-arm64-el2-ras-implementation-tracker-2026-03-15.md` (full RAS tracker)
- `orin-uefi-ras-bug-analysis-2026-03-16.md` (UEFI-side bug, separate issue)

The kernel-side RAS fixes (D2/D3/D3a: frame cap slot identity, PT liveness)
eliminated **runtime** RAS errors.  This work eliminates the **boot-time** RAS
errors from the UEFI-to-elfloader transition.

## Root Cause (from UEFI boot log analysis)

NVIDIA UEFI DEBUG builds poison-fill freed pages with `0xAF` during
`ExitBootServices`.  Some freed pages are intermediate page-table pages still
referenced by `TTBR0_EL2`.  The byte `0xAF` has bit 0 set, so
`0xAFAFAFAFAFAFAFAF` looks like a valid PTE to the hardware walker.  The
walker follows it to physical address `~0xAFAFAFA000`, which the ACI
interconnect cannot decode, producing uncorrectable RAS errors.

Key evidence from `uefi.cap`:
- `TTBR0_EL2 = 0x82cbdc000` falls in EFI BootSvcData region (UEFI's page tables)
- `ELR_EL3 = 0x819aab9a0` resolves to `flush_dcache_range` in the elfloader
- `ELR_EL2 = 0x819aafc3c` resolves to `tegra_tcu_putchar`
- RAS ADDR values: `0x8000afafafafa000` incrementing by `0x40` (cache line stride)
- 130+ identical RAS errors in a single boot

The elfloader never switched away from UEFI's page tables — it ran on the
poisoned `TTBR0_EL2` from `ExitBootServices` until `arm_enable_hyp_mmu()`.

## Implementation

### Iteration 1: Move quiesce call + leave MMU off

Moved `quiesce_uefi_hyp()` from `main()` into `efi_exit_boot_services()`,
immediately after the `bts->exit_boot_services()` call.  Removed the
`enable_mmu` at the end of `quiesce_uefi_hyp` — MMU stays off because UEFI's
page table structure is not trustworthy in DEBUG builds.

**Result:** Zero RAS errors, 141 tests passed.  But the elfloader runs
uncached from ExitBootServices until `arm_enable_hyp_mmu()`, making boot
very slow.

Also discovered that `efi_init.c` was missing `#include <autoconf.h>`, so the
`#if defined(CONFIG_ARCH_AARCH64)` guard was always false — the quiesce call
was silently compiled out.  Fixed by adding the include.

### Iteration 2: Temporary identity map with 1GB blocks (current)

After the quiesce (flush dcache, disable MMU, TLBI, invalidate icache), build
a minimal identity map using the elfloader's own BSS page table pages, then
re-enable MMU+caches.

**Page table layout:**
- L0: `_boot_pgd_up` (not used in final hyp page tables)
- L1: `_boot_pud_temp` (new, dedicated temp page — separate from `_boot_pud_up`
  which `init_hyp_boot_vspace` needs clean)

**Identity map entries (1GB block descriptors at L1):**
- `PUD[0]`:  device memory, PA `0x00000000-0x3FFFFFFF` (covers all MMIO/UART)
- `PUD[2..33]`: normal cached, PA `0x80000000-0x87FFFFFFF` (32GB DRAM)

**MAIR/TCR:** Same values as `arm_enable_hyp_mmu` for consistency.

**Lifecycle:** The temp map is abandoned when `arm_enable_hyp_mmu()` sets
`TTBR0_EL2` to `_boot_pgd_down` with a full TLBI.  Neither `_boot_pgd_up`
nor `_boot_pud_temp` is referenced by the final hyp page tables.

**Why `_boot_pud_temp` instead of `_boot_pud_up`:**
`init_hyp_boot_vspace()` writes `_boot_pgd_down[PGD_idx(kernel_vaddr)]` →
`_boot_pud_up`.  It only writes one entry in `_boot_pud_up` and relies on
zero for the rest.  If we used `_boot_pud_up` for the temp map, our stale 1GB
block entries would persist into the final page tables (reachable via
`_boot_pgd_down`), creating aliased mappings in the kernel VA space.

**Result:** Zero RAS errors, 141 tests passed, boot speed restored (cached).
One unrelated runtime RAS error observed (SCC illegal address + ACI assertion
at PA `0x7fff4580`, below DRAM — likely a sel4test fault-handling test).

## Files Changed

| File | Change |
|------|--------|
| `elfloader-tool/src/binaries/efi/efi_init.c` | Added `#include <autoconf.h>` and `<elfloader/gen_config.h>`.  Moved `quiesce_uefi_hyp()` call here, right after `bts->exit_boot_services()`. |
| `elfloader-tool/src/arch-arm/sys_boot.c` | Removed old `quiesce_uefi_hyp()` call from `main()`, replaced with comment. |
| `elfloader-tool/src/arch-arm/armv/armv8-a/64/mmu-hyp.S` | Rewrote `quiesce_uefi_hyp`: after flush+disable+TLBI+icache, zeros `_boot_pgd_up`/`_boot_pud_temp`, sets up MAIR/TCR, builds 1GB block identity map, sets TTBR, TLBI, enables MMU. |
| `elfloader-tool/src/arch-arm/64/structures.c` | Added `_boot_pud_temp[512]` page-aligned BSS. |
| `elfloader-tool/include/arch-arm/64/mode/structures.h` | Added `extern uint64_t _boot_pud_temp[]` declaration. |

## Test Results

| Run | Description | RAS errors | sel4test result |
|-----|-------------|------------|-----------------|
| `20260316-175009` | Before fix (old quiesce in main, MMU re-enabled with UEFI tables) | 2 (boot-time SCC+ACI) | 141 pass |
| `20260316-180241` | Old quiesce compiled out (missing autoconf.h) | 2 (boot-time SCC+ACI) | 141 pass |
| `20260316-181152` | Fix v2: quiesce in efi_exit_boot_services, MMU off | 0 boot-time | 141 pass |
| `20260316-192501` | Fix v3: temp identity map, MMU+caches re-enabled | 0 boot-time, 1 runtime | 141 pass |

The runtime RAS error in `20260316-192501` is unrelated to the UEFI transition:
- `ELR_EL3 = 0x8080016a08` (seL4 kernel, not elfloader)
- `ESR_EL2 = 0x56000000` (HVC trap, not data abort)
- `VTTBR_EL2 = 0x10000ac002000` (stage-2 active, VM running)
- RAS ADDR = `0x800000007fff4580` (PA below DRAM, interconnect decode error)
- Likely a sel4test fault-handling test probing invalid memory

## Elfloader → Kernel MMU Contract (upstream `master` analysis)

Based on upstream `seL4/master` branches of both `seL4_tools` (elfloader) and
`seL4` (kernel).  No local modifications considered in this section.

### The contract: MMU ON, elfloader's page tables active

The kernel's `_start` in upstream `head.S` does **not** disable the MMU, does
not reload TTBRs, does not do TLBI.  It does a read-modify-write on SCTLR:

```asm
mrs     x4, SCTLR              // read current (inherited from elfloader)
orr     x4, x4, x19            // set CR_BITS_SET (includes CONTROL_M = MMU on)
bic     x4, x4, x20            // clear CR_BITS_CLEAR
msr     SCTLR, x4              // write back — MMU stays on throughout
```

The kernel immediately uses virtual addresses for the stack (`kernel_stack_alloc`)
and calls `init_kernel()`.  The jump itself (`kernel_info.virt_entry`) is a
virtual address that only resolves with the elfloader's page tables active.

### EL2 (CONFIG_ARM_HYPERVISOR_SUPPORT) path

In `continue_boot()`:

```
1. disable_caches_hyp()         — flush D-cache, disable I/D caches (MMU stays on)
2. init_hyp_boot_vspace()       — build _boot_pgd_down:
     _boot_pgd_down[0] → _boot_pud_down → _boot_pmd_down  (elfloader 1:1 identity)
     _boot_pgd_down[1] → _boot_pud_up   → _boot_pmd_up    (kernel VA → PA mapping)
3. arm_enable_hyp_mmu()         — disable MMU, set MAIR/TCR/TTBR0_EL2=_boot_pgd_down,
                                   TLBI, enable MMU
4. Jump to kernel_info.virt_entry with MMU ON
```

EL2 has a single 48-bit VA range (one TTBR).  The elfloader splits it across
`_boot_pgd_down[0]` (low addresses, identity map) and `_boot_pgd_down[1]`
(high addresses, kernel virtual map).

The kernel inherits `TTBR0_EL2 = _boot_pgd_down` and runs on it until
`activate_kernel_vspace()` in `init_cpu()` switches to the kernel's own page
tables.

### EL1 (non-hypervisor) path

In `continue_boot()`:

```
1. leave_hyp()                  — ERET from EL2 to EL1 with MMU OFF
                                   (elfloader does the EL2→EL1 downgrade)
2. init_boot_vspace()           — build two separate PGDs:
     _boot_pgd_down (TTBR0_EL1): elfloader 1:1 identity
     _boot_pgd_up   (TTBR1_EL1): kernel VA → PA mapping
3. arm_enable_mmu()             — disable MMU, set MAIR/TCR,
                                   TTBR0_EL1=_boot_pgd_down,
                                   TTBR1_EL1=_boot_pgd_up,
                                   TLBI, enable MMU
4. Jump to kernel_info.virt_entry with MMU ON
```

EL1 has two VA ranges (TTBR0 for low, TTBR1 for high).  The kernel virtual
address falls in the TTBR1 range.

**Key:** `leave_hyp()` is called **before** any page table setup.  The
EL2→EL1 downgrade happens with MMU off.  The **elfloader** does the
downgrade, not the kernel.

### Who downgrades EL2→EL1?

The **elfloader** does, in `leave_hyp()`.  This is gated on:

```c
#if !defined(CONFIG_ARM_HYPERVISOR_SUPPORT)
    if (is_hyp_mode()) {
        leave_hyp();   // ERET to EL1
    }
#endif
```

So if the bootloader starts at EL2 but `CONFIG_ARM_HYPERVISOR_SUPPORT` is not
set, the elfloader drops to EL1 before building page tables.  The kernel
always enters at the EL it was configured for.

### What upstream doesn't do (and should, on Orin)

Upstream has **no cleanup at any transition point**:

| Gap | Detail |
|-----|--------|
| No UEFI→elfloader TLBI | Stale UEFI TLB entries persist until `arm_enable_hyp_mmu()` |
| No TLBI in kernel `_start` | Stale elfloader TLB entries persist until `activate_kernel_vspace()` |
| No DSB/ISB after SCTLR write in `_start` | Architecturally required for SCTLR changes to take effect |
| Read-modify-write SCTLR | Inherits unknown bits from elfloader instead of fresh write |
| No TTBR reload in `_start` | Relies on elfloader's TTBR being correct |

This works on platforms where:
- UEFI's page tables don't get corrupted after ExitBootServices (RELEASE builds)
- The elfloader's page tables are exactly what the kernel needs
- No speculative prefetch through stale TLB entries into freed memory

On Orin AGX with UEFI DEBUG, it breaks because the hardware walker
speculatively prefetches through poisoned page tables.

### Are our temporary mappings compatible with the contract?

**Yes.**  The temp map is completely replaced before the kernel sees anything:

```
1. quiesce_uefi_hyp()     → TTBR0_EL2 = _boot_pgd_up (temp identity map)
2. load_images(), etc.     — run cached on temp map
3. disable_caches_hyp()    — disable caches, MMU stays on with temp map
4. init_hyp_boot_vspace()  — build final tables in _boot_pgd_down
5. arm_enable_hyp_mmu()    — disable MMU, TTBR0_EL2 = _boot_pgd_down, TLBI, enable MMU
6. Jump to kernel          — kernel sees _boot_pgd_down, never sees temp map
```

The contract — **MMU on, `_boot_pgd_down` in TTBR, identity + kernel VA
mapping active** — is preserved exactly as upstream intended.  The elfloader
map only exists during elfloader execution (steps 1–4) and is abandoned with
a full TLBI in step 5.

`_boot_pgd_up` and `_boot_pud_elfloader` are not reachable from
`_boot_pgd_down`, so no stale entries leak into the final page tables.

## Current Implementation (commit series)

```
a590028  arm64/efi: quiesce UEFI MMU/TLB state after ExitBootServices
550f5e9  arm64/efi: build temp identity map from EFI memory map after ExitBootServices
76c869c  arm64: generic temp identity map from ELF headers, split quiesce/enable
51246e9  arm64: rename quiesce_uefi_hyp to quiesce_hyp_mmu, call on all boot paths
45ecce0  arm64: rename temp_identity_map to elfloader_map
25f08ce  arm64: rename _boot_pud_temp to _boot_pud_elfloader
```

Final naming:

| Symbol | Purpose |
|--------|---------|
| `quiesce_hyp_mmu` | Flush/disable/TLBI — generic, all boot paths |
| `build_elfloader_map` | Build identity map from ELF headers + UART |
| `enable_elfloader_map_hyp` | Set MAIR/TCR/TTBR, enable MMU+caches |
| `_boot_pgd_up` | Elfloader's L0 page table |
| `_boot_pud_elfloader` | Elfloader's L1 page table |

The sequence in `main()` for all AArch64 EL2 paths:

```
1. quiesce_hyp_mmu()           — flush, disable MMU, TLBI, icache
2. build_elfloader_map()       — populate _boot_pgd_up + _boot_pud_elfloader
3. enable_elfloader_map_hyp()  — MAIR/TCR, TTBR, enable MMU+caches
4. load_images()               — runs cached
5. relocate_below_kernel()     — runs cached
```

For EFI, `quiesce_hyp_mmu()` is also called early in
`efi_exit_boot_services()` to stop the walker immediately after
ExitBootServices.  The second quiesce in step 1 is idempotent.

## Remaining Work

### Elfloader (this scope)
- [x] Move quiesce into `efi_exit_boot_services()` (minimal C window)
- [x] Build elfloader identity map from ELF headers + UART
- [x] Re-enable MMU+caches for performance
- [x] Generic: works on all AArch64 EFI platforms (no hardcoded addresses)
- [x] Generic: quiesce called on all boot paths, not just EFI
- [x] Split quiesce (disable) from enable (build map + switch TTBR)
- [ ] Investigate runtime RAS error at PA `0x7fff4580` (likely sel4test, not elfloader)

### EL1 path (not implemented, known gap)

The current elfloader identity map (`build_elfloader_map` +
`enable_elfloader_map_hyp`) only covers the EL2 (hypervisor) path.
The EL1 (non-hypervisor) path is not covered.

**Why it's lower priority:**

1. The EL1 path does not have the UEFI poison problem.  `leave_hyp()` already
   does flush + disable MMU + TLBI at EL2 before the EL2→EL1 ERET, and the
   EL1 MMU was never enabled (UEFI runs at EL2).  There are no stale EL1 TLB
   entries.

2. The EL1 path runs `load_images()` uncached (same as the EL2 path did before
   this work).  An elfloader identity map for EL1 would improve performance,
   but is not required for correctness.

3. EL1 needs two TTBRs (TTBR0_EL1 for low addresses, TTBR1_EL1 for high),
   making the page table setup slightly different from the single-TTBR EL2
   case.  Not hard, but more code.

4. Most AArch64 seL4 deployments use `CONFIG_ARM_HYPERVISOR_SUPPORT` (EL2).
   The EL1 path is mainly for older or constrained platforms.

**If implemented later**, it would need:
- `quiesce_el1_mmu` or extend `quiesce_hyp_mmu` with an EL1 variant
- `enable_elfloader_map_el1` that sets both TTBR0_EL1 and TTBR1_EL1
- The identity map itself can reuse `build_elfloader_map()` — only the
  assembly switch sequence differs

### Kernel head.S (Phase 2, separate scope)
- [ ] Quiesce elfloader's MMU/TLB state at `_start` (plan in `orin-elfloader-kernel-mmu-quiesce-plan-2026-03-16.md`)
- [ ] Fresh SCTLR write instead of read-modify-write

### End-to-end zero RAS
- [x] Boot-time UEFI→elfloader: zero RAS (this work)
- [x] Runtime kernel vspace ops: zero RAS (D2/D3/D3a frame cap slot identity)
- [ ] Boot-time elfloader→kernel: plan exists, not yet implemented
- [ ] EL1 elfloader identity map: known gap, not needed for correctness
- [?] Runtime sel4test fault tests: 1-2 RAS errors, needs investigation
