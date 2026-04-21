# [PATCH 0/4] arm64: Fix cache maintenance for EL2 and boot

This series fixes three cache maintenance bugs on arm64 and adds
translation object sanitisation on deletion. All four patches are
independent and can be applied or dropped individually, except that
patch 4 (PoU→PoC) should come after patch 3 if both are applied.

## The bugs

### 1. Set/way cache ops at boot (patch 1)

`clean_D_PoU()`, `cleanInvalidate_D_PoC()`, and `cleanInvalidate_L1D()`
use `dc csw`/`dc cisw` (set/way) loops. These are architecturally
broken on arm64:

- Race with speculative fetches (ARM DDI 0487 D4.4.1: set/way ops
  affect caches "private to the PE"; speculative refills can occur
  between the op and the completion DSB).
- Not broadcast in SMP — only affect the executing PE's caches.
- Don't reach system-level caches (L3, SLC) which only respect
  VA-based operations.

Linux arm64 removed `flush_cache_all()` in 2015 for these reasons.
Replace with `cleanInvalidateCacheRange_RAM()` (dc civac) on the
boot memory region.

### 2. DMB after cache maintenance (patch 2)

`cleanByVA()` (dc cvac), `cleanByVA_PoU()` (dc cvau), and
`invalidateByVA()` (dc ivac) use `dmb()` as the completion barrier.
ARM DDI 0487 B2.3.5: DMB orders memory accesses but does **not**
guarantee completion of cache maintenance. DSB is required. Change
`dmb()` → `dsb()`. `cleanInvalByVA()` (dc civac) already uses `dsb()`.

### 3. PoU cache maintenance for page tables (patch 4)

All page table cache maintenance uses `dc cvau` (clean to PoU). The
ARM architecture defines two different coherency points for translation
table walks (ARM DDI 0487 D8.3):

- **Stage-1 walks**: coherent at PoU within the Inner Shareable domain.
- **Stage-2 walks**: coherent at PoC within the Inner Shareable domain.

When seL4 runs at EL2 (`CONFIG_ARM_HYPERVISOR_SUPPORT`), native
threads use stage-2 only:

```
HCR_NATIVE = HCR_VM | ... | HCR_DC | HCR_TGE | ...
```

`HCR_EL2.DC=1` disables stage-1; `HCR_EL2.VM=1` enables stage-2.
The stage-2 walker requires PoC coherency, making `dc cvau`
architecturally insufficient.

Since PoC is a superset of PoU (always correct for stage-1 too), the
fix is unconditional — no `#ifdef` needed. This matches Linux arm64
(`arch/arm64/include/asm/cacheflush.h`).

**Empirical evidence**: on NVIDIA Orin AGX (Cortex-A78AE) at EL2,
sel4test with PoU-only maintenance crashes during CNODEOP tests
(capability revocation recycles page table memory; the stage-2 walker
sees stale PTEs). With PoC, 141/141 tests pass across 4 runs.

## The series

```
[1/4] arm64: replace set/way boot cache ops with VA-based operations
[2/4] arm64: fix cache maintenance completion barriers
[3/4] arm64: flush translation object cache on final delete
[4/4] arm64: use PoC cache maintenance for page table operations
```

### Patch 1: arm64: replace set/way boot cache ops with VA-based operations

`src/arch/arm/armv/armv8-a/64/cache.c`

Replace `dc csw`/`dc cisw` set/way loops with VA-based
`cleanInvalidateCacheRange_RAM()` on the boot memory region.
Boot-time only, no runtime behavior change.

### Patch 2: arm64: fix cache maintenance completion barriers

`include/arch/arm/arch/64/mode/machine.h`

`dmb()` → `dsb()` after dc cvac, dc cvau, dc ivac. Minimal fix.

### Patch 3: arm64: flush translation object cache on final delete

`src/arch/arm/64/object/objecttype.c`

Add `cleanCacheRange_PoU` on VSpace and PageTable final deletion in
`Arch_finaliseCap`. Ensures page table memory is cache-clean before
the allocator reuses it. Same cache maintenance as the existing
creation path. No zeroing, no new helper — just the flush.

### Patch 4: arm64: use PoC cache maintenance for page table operations

`include/arch/arm/arch/machine.h`
`src/arch/arm/64/kernel/vspace.c`
`src/arch/arm/64/object/objecttype.c`

Change all page table maintenance from PoU (`dc cvau` /
`cleanCacheRange_PoU`) to PoC (`dc civac` /
`cleanInvalidateCacheRange_RAM` / `cleanInvalByVA`). Includes both
the existing creation/map paths and the new deletion path from
patch 3.

## Testing

All testing on NVIDIA Orin AGX (Cortex-A78AE), seL4 at EL2, single
core.

- Full series: sel4test 141/141 pass, 0 RAS errors (4 runs)
- Patches 1-3 without patch 4: sel4test crashes at CNODEOP0003
  (confirms patch 4 is a correctness fix, not hardening)
- Patches 1-2 only: sel4test 141/141 pass (confirms patches 1-2
  are independently safe)

## References

- ARM DDI 0487 (ARM Architecture Reference Manual for A-profile):
  - B2.3.5: DMB vs DSB for cache maintenance completion
  - D4.4.1: Set/way cache maintenance limitations
  - D8.3: Translation table walk coherency (PoU for stage-1, PoC for stage-2)
  - D13.2.46: HCR_EL2.DC effect on stage-1 translation
- Linux arm64 flush_cache_all() removal: commit 2cf3a1fc60 (2015)
- Linux arm64 cache maintenance: `arch/arm64/include/asm/cacheflush.h`
- Detailed analysis: `docs/architecture/arm64-el2-cache-maintenance-poc.md`
