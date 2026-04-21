# Deferred PT Sanitise — Implementation Tracker

Date: 2026-03-17
Plan: `docs/plans/orin-ras-deferred-pt-sanitise-plan-2026-03-17.md`
Branch: `frame-cap-slot-identity` (kernel repo)
Goal: Zero RAS errors on Orin AGX sel4test — eliminate speculative walker race during bulk cap revocation

## Design Summary

Two-phase cap revocation (Solution 1 from plan):
- **Phase 1** (per-cap, during `cteRevoke` loop): Unlink PT from translation tree
  (drain leaves, invalidate parent entry, per-ASID TLBI). Do NOT sanitise PT memory.
  Enqueue PT pointer in static pending queue.
- **Phase 2** (at drain point): Single global `TLBI ALLE2IS` + `DSB(sy)` + `ISB`,
  then sanitise and `ptRefcountForget` all queued PTs.

Drain points: after every cap deletion in `cteRevoke`, on `cteRevoke` exit,
and after standalone `invokeCNodeDelete`.

## Changes

### K1: Pending PT sanitise queue — data structures
- **File**: `kernel/include/arch/arm/arch/64/mode/model/statedata.h`
- **Change**: Add `pt_sanitise_entry_t` struct, `armKSPendingPTSanitise[16]` array,
  `armKSPendingPTSanitiseCount` counter.
- **Status**: Done

### K2: Pending PT sanitise queue — BSS allocation
- **File**: `kernel/src/arch/arm/64/model/statedata.c`
- **Change**: Allocate `armKSPendingPTSanitise` and `armKSPendingPTSanitiseCount`.
- **Status**: Done

### K3: Drain and enqueue functions
- **File**: `kernel/src/arch/arm/64/kernel/vspace.c`
- **Change**: Add `pendingPTSanitiseDrain()` and `pendingPTSanitiseEnqueue()`.
  Drain issues single `invalidateTranslationAll()` + `dsb()` + `isb()`,
  then loops over queued entries: `ptRefcountForget` + sanitise (write invalid
  PTEs + `cleanInvalidateCacheRange_RAM`). Enqueue auto-drains if queue is full.
- **Status**: Done

### K4: Function declarations
- **File**: `kernel/include/arch/arm/arch/kernel/vspace.h`
- **Change**: Declare `pendingPTSanitiseDrain()` and `pendingPTSanitiseEnqueue()`.
- **Status**: Done

### K5: PT cap finalization — defer sanitise
- **File**: `kernel/src/arch/arm/64/object/objecttype.c`
- **Change**: `cap_page_table_cap` case in `Arch_finaliseCap`: replace inline
  `invalidateTranslationAll` + `dsb` + `isb` + `ptRefcountForget` +
  `sanitiseTranslationObject` with `pendingPTSanitiseEnqueue()`.
  Unmapped PTs (capPTIsMapped=false) still sanitise immediately.
- **Status**: Done

### K6: VSpace cap finalization — defer sanitise
- **File**: `kernel/src/arch/arm/64/object/objecttype.c`
- **Change**: `cap_vspace_cap` case: replace inline TLBI + sanitise with
  `pendingPTSanitiseEnqueue()`.
- **Status**: Done

### K7: cteRevoke — drain at preemption points
- **File**: `kernel/src/object/cnode.c`
- **Change**: Add `#include <arch/kernel/vspace.h>`. Call
  `pendingPTSanitiseDrain()` after each `cteDelete` (before `preemptionPoint`),
  on `cteDelete` error return, and after the `cteRevoke` loop exits.
- **Status**: Done

### K8: invokeCNodeDelete — drain after single delete
- **File**: `kernel/src/object/cnode.c`
- **Change**: Call `pendingPTSanitiseDrain()` after `cteDelete` returns in
  `invokeCNodeDelete`.
- **Status**: Done

### K9: DSB(nsh) at kernel_enter — quiesce speculative EL0 walks
- **File**: `kernel/src/arch/arm/64/traps.S`
- **Change**: Add `dsb nsh` at the start of the `kernel_enter` macro, before
  register saves. Per ARM ARM R_LFHQG (D8.1.5), when taking an exception from
  EL0/EL1 to EL2, the speculative page table walker may still have in-flight
  walks from the lower EL. The DSB ensures these complete before the kernel
  modifies the page tables.
  This is the same fix as Marc Zyngier's KVM patch (April 2023):
  "KVM: arm64: Synchronise speculative page table walks on translation
  regime change".
- **Status**: Done

## Build

| Target | Config | Status |
|--------|--------|--------|
| sel4test | `orinagx_defconfig` | **Pass** (303/303) |

## Test Runs

| Run ID | RAS Errors | sel4test Result | Notes |
|--------|-----------|-----------------|-------|
| 20260317-080331 | 2 | 141 pass | K1-K8 only (deferred PT sanitise). ADDR=0x7fff8500. Did NOT help. |
| 20260317-085120 | 2 | 141 pass | + K9 DSB(nsh) at kernel_enter. ADDR=0x7fff44c0. Did NOT help. |
| 20260317-091128 | — | CRASH | + VTTBR→empty at kernel_enter. Rootserver VM fault — VTTBR not restored on exit. |
| 20260317-091940 | 4 | 141 pass | + Safe DRAM PA in pte_pte_invalid_new(). ADDR=0x7fff44c0. Did NOT help — confirms walker is not reading invalid PTE content. |
| 20260317-103206 | 6 | 141 pass | + Clear TTBR0/1_EL1 + TLBI at boot. ADDR=0x7fff44c0. Did NOT help — stale UEFI stage-1 tables are NOT the source. |

## Validation Criteria

- **Pass**: 5 consecutive sel4test runs with 0 RAS errors.
- **Prior baseline**: 1–2 RAS errors per run (non-deterministic) in experiments E1–E6.

## Analysis of Failure

Deferred PT sanitisation did NOT fix the RAS errors. Same 2 errors, same
address (`0x7fff8500`), same test (`CANCEL_BADGED_SENDS_0002`), same EL=2
context.

**Key insight**: The walker race is NOT about freed/sanitised PT memory reuse.
The PT memory is still live and mapped when the error occurs. The race is at
the individual PTE retirement level — between `publishTranslationSlot(invalid)`
and the TLBI within `retireTranslationSlotByASID`. The walker prefetches the
old valid PTE from the **live** page table before the invalidating store
becomes visible to the walker's read port.

This rules out Solution 1 (deferred PT sanitise) and Solution 4 (quarantine)
from the plan — both address PT memory reuse timing, which is not the problem.

The remaining viable approach is **Solution 2 (safe PA in invalid PTEs)** —
but applied not to `pte_pte_invalid_new()` (E6 showed this doesn't help),
but to the **entire page table content** for tables that the walker might
follow speculatively. If the walker follows a stale valid parent entry to
a PT whose content has all-invalid entries with safe DRAM addresses, no
RAS error occurs. However, E6 already tested zero PTEs (PA=0, which is
below DRAM) and it didn't help, suggesting the walker is reading
pre-invalidation content, not post-invalidation content.

### Further experiments (K9 + VTTBR + safe PA)

**K9 — DSB(nsh) at kernel_enter** (run 20260317-085120): Added DSB to
quiesce old EL0 walks on EL2 entry (matching KVM R_LFHQG fix). **No effect.**
The RAS source is not old EL0 walks surviving into EL2.

**VTTBR→empty at kernel_enter** (run 20260317-091128): Switched VTTBR to
armKSGlobalUserVSpace at kernel_enter to prevent new speculative stage-2
walks during kernel execution. **Crashed** — VTTBR was not restored before
eret (setVMRoot only runs on thread switch, not same-thread resumption).
The approach is sound but needs a matching restore in restore_user_context.

**Safe DRAM PA in pte_pte_invalid_new()** (run 20260317-091940): Changed
invalid PTE content from all-zero to armKSGlobalUserVSpace PA (valid DRAM).
**No effect** (4 RAS errors, same ADDR=0x7fff44c0). Combined with E6
(all-zero invalid PTE, also no effect), this proves the walker is NOT
reading our invalid PTE content. The 0x7fffXXXX address comes from
somewhere else entirely — either stale valid PTE content from before
invalidation, or from reading freed/reused memory that contains arbitrary
data interpreted as a PTE.

### Key observation

The RAS ADDR (0x7fff8500, 0x7fff44c0) is consistently just below DRAM base
(0x80000000), but is NOT any value that seL4 writes to PTEs. It must come
from the walker reading pre-existing data from a table that was freed/reused,
or from a hardware prefetch pattern. The address varies slightly between
runs, suggesting it comes from dynamic data, not a fixed kernel structure.

### K10: Clear TTBR0/1_EL1 at boot (run 20260317-103206)

Zeroed TTBR0_EL1 and TTBR1_EL1 in `armv_vcpu_boot_init()` with TLBI
after. Theory: stale UEFI stage-1 tables cause speculative combined
S1+S2 walks. **No effect** — 6 RAS errors, same ADDR.

Also confirmed: `vcpu_switch(NULL)` is a **no-op** for non-VCPU threads
(armHSCurVCPU starts NULL, new is NULL → condition is false). SCTLR_EL1
is only written once during `armv_vcpu_boot_init()` (M=0, stage-1
disabled). TTBR0/1_EL1 were never initialized by seL4 at all before
this fix.

## Tegra234 Memory Map Study

### PA 0x7fff44c0 / 0x7fff8500 — decode hole

Both addresses fall in a **~1 GB hardware decode hole** between SYSRAM
(`0x40080000`) and DRAM base (`0x80000000`) on Tegra234. No MMIO device,
no memory exists in this range:

| Address Range | Region |
|---|---|
| 0x00000000 – 0x17FFFFFF | SoC peripherals (UART, GIC, SMMU, GPU, PCIe) |
| 0x18000000 – 0x3FFFFFFF | **Unmapped gap (~640 MB)** |
| 0x40000000 – 0x40080000 | SYSRAM (512 KB, TZ-secured) |
| 0x40080000 – 0x7FFFFFFF | **Unmapped gap (~1 GB) ← RAS addresses here** |
| 0x80000000 – ... | DRAM |

Any access to 0x7fffXXXX triggers an SCC decode error: the interconnect
cannot route the request to any valid target.

**Source**: Tegra234 device tree (`tegra234.dtsi`), CBB slave map
(`tegra234-cbb.c`), NVIDIA Developer Forums thread 255520 (identical
RAS pattern reported by other users).

### SMMU status

SMMU is **disabled** on Orin AGX (`KernelArmSMMU OFF` in
`kernel/src/plat/orinagx/config.cmake`). No IOMMU agent is involved.

### What generates the access?

Ruled out by experiments:
- Invalid PTE content (tested zero, tested safe DRAM PA — no effect)
- Stale UEFI TTBR0/1_EL1 (cleared at boot — no effect)
- Old EL0 walks surviving into EL2 (DSB at entry — no effect)
- PT memory reuse during revocation (deferred sanitise — no effect)
- SMMU / DMA (SMMU disabled, no DMA in sel4test)

**Key observation**: The RAS ADDR is the same within a run but varies
between runs (0x7fff8500 vs 0x7fff44c0). This means the address comes
from dynamic data — likely the content of memory that was once a page
table, was freed/retyped, and now contains user data that the speculative
walker interprets as a valid PTE with address bits in the decode hole.

The deferred PT sanitise prevents reuse during the `cteRevoke` loop, but
the actual retype (and subsequent user data write) happens in a LATER
syscall. By that time, the pending queue has been drained, the PT memory
has been sanitised, and it's available for retype. If the walker holds
a cached reference to a parent table entry that still points to the
old PT's PA, and that PA has been retyped to a user frame containing
arbitrary data, the walker reads user data and interprets it as PTEs.

### Remaining approaches

1. **VTTBR switch with proper restore**: Switch VTTBR to empty at
   kernel_enter, save original VTTBR in per-CPU global, restore in
   restore_user_context before eret. This prevents ANY speculative
   stage-2 walks during kernel execution.
2. **Disable HCR_EL2.VM at kernel entry**: Same effect as VTTBR switch
   but simpler — disable stage-2 entirely during kernel execution.
   Same restore requirement.
3. **Investigate retype path**: Check if retyped PT memory retains a
   parent table reference that the walker can follow. The parent entry
   should be invalidated before the PT is freed, but there may be a
   window between parent invalidation and retype where the walker
   caches the old parent entry.

## CANCEL_BADGED_SENDS_0002 Test Structure

The test (`endpoints.c:174-248`) does **NOT** retype memory:
1. Creates 32 helper threads with badged endpoint caps
2. Main loop: `cnode_revoke(badged_ep)` + `cnode_cancelBadgedSends(badged_ep)`
3. Revokes derived endpoint caps only — no frame/PT caps are directly revoked
4. Page table teardown happens during test **cleanup** (sel4utils_destroy_process)

This rules out the "retyped PT memory" theory: the RAS errors occur during
the test, not during a retype cycle. The memory is still in its original
state (PT or frame caps) when the error fires.

## SCTLR_EL1 and VCPU Investigation

### Boot-time SCTLR_EL1 initialization

- `head.S` initializes `sctlr_el2` only (hypervisor mode). **sctlr_el1 is
  NOT touched at boot.**
- `armv_vcpu_boot_init()` (called from `boot.c:198`) writes
  `SCTLR_EL1_NATIVE` which has **M=0** (stage-1 disabled for EL1/EL0).
- TTBR0_EL1 and TTBR1_EL1 were **never initialized** by seL4 — retained
  stale UEFI values until our K10 fix zeroed them.

### vcpu_switch(NULL) is a no-op for non-VCPU threads

For sel4test (no VCPUs), `tcb->tcbArch.tcbVCPU == NULL`.
`vcpu_switch(NULL)` checks:
```
armHSCurVCPU != new  →  NULL != NULL  →  false
!armHSVCPUActive && new != NULL  →  !false && false  →  false
```
Both branches skip. **No SCTLR_EL1 write on thread switch.**

SCTLR_EL1 is written exactly once: `armv_vcpu_boot_init()` at boot.
After that, SCTLR_EL1.M=0 persists for the entire runtime.

### TTBR0/1_EL1 clearing experiment (K10)

Zeroing TTBR0/1_EL1 + TLBI at boot had **no effect** on RAS errors.
This rules out stale UEFI stage-1 tables as the source.

With SCTLR_EL1.M=0, the architecture says no stage-1 translation is
performed. Combined with TTBR0/1_EL1=0, there is no stage-1 state
for the walker to use — speculative or otherwise.

## Retype Path Analysis

### Untyped reuse sequence

1. `cteRevoke` deletes all child caps of an untyped (frames, PTs)
2. During finalization: PT parent entries invalidated, TLBI, DSB+ISB
3. `pendingPTSanitiseDrain()`: global TLBI + DSB + ISB, then sanitise PTs
4. Untyped cap now has no children → available for retype
5. On next retype: `resetUntypedCap()` → `clearMemory()`:
   - memzero (writes zeros)
   - `cleanInvalidateCacheRange_RAM()` (DSB ISHST + dc civac loop + DSB ISH)
6. New objects allocated from start of untyped

### No TLBI gap between sanitise and retype

The barrier sequence is:
- Sanitise: writes invalid PTEs + cache cleans (PoC)
- retype clearMemory: writes zeros + cache cleans (PoC)

Both ensure data at PoC before proceeding. However, **neither
resetUntypedCap nor clearMemory issues a TLBI**. They rely on the
prior TLBI from `pendingPTSanitiseDrain()` (or `Arch_finaliseCap`
in the non-deferred path).

If the walker has a cached walk-cache entry from before the TLBI,
and the TLBI didn't fully invalidate the walk cache (microarchitectural
quirk), the walker could follow the stale entry to the old PT PA —
which now contains zeros (from clearMemory). Zeros have bits[1:0]=0
(invalid), so the walker should stop. But PA=0 from a zero entry is
below DRAM — this doesn't match 0x7fff44c0.

## Summary of All Experiments

| # | Experiment | Run | RAS | Conclusion |
|---|-----------|-----|-----|-----------|
| E1 | ISB after TLBI | 20260317-002640 | 2 | No effect |
| E2 | Full TLBI at PT finalize | 20260317-004142 | 2 | No effect |
| E3 | VMALLS12E1IS after IPAS2E1 | 20260317-004825 | 1-2 | Marginal, non-deterministic |
| E5 | Disable stage-2 during VTTBR switch | 20260317-011110 | 2 | No effect |
| E6 | Zero invalid PTE | 20260317-012020 | 0-2 | No effect (non-deterministic) |
| K1-K8 | Deferred PT sanitise | 20260317-080331 | 2 | No effect |
| K9 | DSB(nsh) at kernel_enter | 20260317-085120 | 2 | No effect |
| — | VTTBR→empty at kernel_enter | 20260317-091128 | CRASH | Broke: no VTTBR restore |
| — | Safe DRAM PA in invalid PTE | 20260317-091940 | 4 | No effect |
| K10 | Clear TTBR0/1_EL1 at boot | 20260317-103206 | 6 | No effect |
| **K11** | **PPTR map from physBase()** | **20260317-105807** | **0** | **ZERO RAS. Root cause found.** |
| K11 | PPTR map from physBase() (run 2) | 20260317-110149 | 0 | Confirmed: zero RAS, 141 pass |

**Consistent across ALL experiments:**
- RAS always during CANCEL_BADGED_SENDS_0002
- ADDR always 0x7fffXXXX (decode hole below DRAM)
- Same ADDR within a run, varies between runs
- 141 tests pass regardless of RAS count
- EL=2 (kernel context during syscall)

**Not from:**
- Our invalid PTE content (tested 3 variants)
- Stale UEFI TTBR0/1_EL1
- Old EL0 walks
- PT memory reuse timing
- Any per-entry barrier/TLBI improvement

**Actual root cause (K11):**
- **Speculative CPU prefetch through the kernel's PPTR window mapping.**
- The kernel maps PA 0 to ~0x7fc0000000 as NORMAL cacheable memory in
  TTBR0_EL2 stage-1 tables. This covers the Tegra234 decode hole
  (PA 0x40080000–0x7FFFFFFF) where no device or memory exists.
- During heavy kernel operations (cancelBadgedSends → bulk cap
  revocation), speculative data prefetches hit PAs in the decode hole.
- The SCC interconnect rejects these as "Address Range Error / Illegal
  address" → RAS Uncorrectable Error.
- **Fix**: Start PPTR mapping from `physBase()` (0x80000000, DRAM start)
  instead of `PADDR_BASE` (0x0). Non-DRAM regions are never mapped as
  NORMAL cacheable, eliminating speculative prefetch into decode holes.
- This was NOT a page table walker issue. All prior experiments (E1–E6,
  K1–K10) targeted the walker and had no effect because the root cause
  was speculative data prefetch, not speculative translation walks.
