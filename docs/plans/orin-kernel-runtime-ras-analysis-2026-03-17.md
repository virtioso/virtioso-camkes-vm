# Kernel Runtime RAS Analysis

Date: 2026-03-17
Status: Investigation
Scope: seL4 kernel AArch64 EL2 vspace operations on Orin AGX

## Problem

After eliminating all boot-time RAS errors (elfloader quiesce + identity map),
1-2 runtime RAS errors remain per sel4test run.  They are non-deterministic —
some runs have zero, some have 1-2.

## Error Signature

```
ELR_EL3:  0x8080016a08, 0x808001d384  (kernel virtual addresses)
ESR_EL2:  0x56000000  (HVC trap — sel4test syscall in progress)
VTTBR_EL2: non-zero (stage-2 active)
RAS source: SCC (illegal address) + ACI (assertion failure / decode error)
RAS ADDR:  0x7FFFxxxx range (just below DRAM at 0x80000000)
```

The faulting physical addresses are below DRAM — the interconnect cannot
decode them.  These come from the speculative page table walker following
a PTE that contains address bits pointing below DRAM.

## Root Cause: Speculative Walker vs PTE Retirement

The Cortex-A78AE has an out-of-order speculative page table walker.
When the kernel retires a PTE (unmap), the walker can race with the
store+TLBI sequence.

### Upstream unmapPage (master)

```c
*(lu_ret.ptSlot) = pte_pte_invalid_new();        // 1. raw store
cleanByVA_PoU(ptSlot, paddr);                     // 2. clean to PoU (not PoC!)
invalidateTLBByASIDVA(asid, vptr);                 // 3. TLBI
                                                   // 4. NO DSB after TLBI
```

### Our unmapPage (frame-cap-slot-identity branch)

```c
publishTranslationSlot(slot, pte_pte_invalid_new());  // 1. volatile store + dc civac + dsb
invalidateTLBByASIDVA(asid, vaddr);                    // 2. TLBI
dsb();                                                  // 3. DSB after TLBI
```

### Both have the same fundamental issue

The TLBI tells the TLB to discard the old entry. But the walker may have
**already fetched the old valid PTE into its internal pipeline** before the
TLBI arrives.  On an out-of-order core, the sequence is:

```
CPU:    store invalid PTE → dc civac → dsb → TLBI → dsb
Walker: [already prefetched old valid PTE] → follows stale table chain
        → reads next-level PTE from freed/reused memory
        → finds address 0x7FFFxxxx → ACI decode error
```

The store of the invalid entry (bits[1:0]=0) prevents NEW walks from
following the entry.  But a walk that **started before the store became
visible** can still be in-flight.

### pte_pte_invalid_new() content

```c
addrFromPPtr(armKSGlobalUserVSpace) & 0xfffffffff000ull
```

This is not zero — it contains the kernel's global user vspace physical
address in bits[63:12] with bits[1:0]=0 (invalid).  A speculative walker
that reads this as a partially-updated entry might interpret the non-zero
address bits.

## Questions to Resolve

1. **Is the walker truly speculative across TLBI?** — Can a walk that
   started before TLBI complete after TLBI?  ARM ARM D8.14.2 says TLBI
   is "not guaranteed to have taken effect" until DSB+ISB.  But does the
   walker abort in-flight walks on TLBI, or let them complete?

2. **Is PoU sufficient for walker visibility?** — Upstream uses
   `cleanByVA_PoU` but the walker reads from PoC on Orin.  Our branch
   fixed this to `dc civac` (PoC).  If PoU != PoC on Orin, the walker
   might not see the invalid entry at all.

3. **Does the walker follow invalid entries?** — If bits[1:0]=0, the
   walker should not follow the entry.  But if it already loaded the
   OLD entry (bits[1:0]=11, valid table) into its pipeline, it will
   follow that old entry to a next-level table that may have been freed
   and reused.

4. **Are the 0x7FFFxxxx addresses from stage-1 or stage-2?** — VTTBR
   is active, so the walker does stage-1 + stage-2.  The bad address
   could be from either stage's PTE containing stale bits.

## Relationship to Prior Kernel Fixes

The D2/D3/D3a commits (frame cap slot identity, PT liveness) changed
HOW the kernel finds and retires PTEs.  They did not change the
fundamental store→TLBI ordering or barrier strength.  The race window
is the same — just hit less frequently because the frame cap redesign
eliminated redundant walks through live tables.

## Experiments

### E1: ISB after TLBI completion (kernel commit 5a71a8943)

Added `isb()` after `dsb()` in all three retirement helpers.

Result: **no effect**. 2 runtime RAS errors, same PA `0x7fff4540`.
ISB synchronizes the CPU pipeline but does not abort in-flight
speculative walks that already started before TLBI.

Test: `20260317-002640`, 141 pass, 2 RAS.

### E2: Full TLBI at PT/VSpace finalize (kernel commit 7998c720a)

Added `invalidateTranslationAll` + `dsb` + `isb` at PT cap and VSpace
cap finalization, before `sanitiseTranslationObject` and reuse.

Result: **no effect**. 2 runtime RAS errors, same test
(CANCEL_BADGED_SENDS_0002), same PA `0x7fff4540`.

Key insight: the errors happen **during** the bulk revocation loop
(between test start and test pass), not after PT finalize. The walker
race is in the leaf PTE retirement path (`unmapPage` per-VA TLBI)
during rapid-fire bulk unmap, not in the PT memory reuse path.

Test: `20260317-004142`, 141 pass, 2 RAS.

### E3: TLBI VMALLS12E1IS after IPAS2E1 (kernel commit 2238d2b6e)

Added `TLBI VMALLS12E1IS` after `TLBI IPAS2E1` + `DSB` in
`invalidateLocalTLB_IPA_VMID` per ARM ARM D8.10.2.  Flushes cached
combined S1+S2 entries that used the now-invalidated stage-2 PA.

Finding from E2 context: the RAS fires at **EL0** (user process
running), not EL2.  SPSR_EL3 shows EL=0.  The walker is doing a
user-space S1+S2 walk under the restored VTTBR after the kernel
finished the unmap syscall.  The stale combined S1+S2 entry in the
TLB points to the old PA (0x7FFFxxxx).

First attempt used `TLBI VMALLE1IS` (stage-1 only) — reduced from
2 to 1 RAS.  Upgraded to `TLBI VMALLS12E1IS` (stage-1 + combined
S1+S2) — still 1 RAS in first run, 2 in second.  Non-deterministic.

The VMALLS12E1IS is architecturally correct per D8.10.2 and reduces
the race window, but doesn't fully close it.  The remaining errors
may be from a different walker path (e.g. a walk initiated DURING
the VTTBR switch, between the `IPAS2E1` and the `VMALLS12E1IS`).

Tests: `20260317-004825` (1 RAS), `20260317-005238` (1 RAS),
`20260317-005631` (2 RAS).

### E5: Disable stage-2 during VTTBR switch (kernel commit 3f29f405b)

Temporarily clear HCR_EL2.VM before switching VTTBR for TLBI, restore
after TLBI + DSB and VTTBR restore.  Prevents speculative stage-2
walks during the VMID context switch window.

Result: **no effect**. 2 RAS errors, same test.

The walker race is NOT in the VTTBR switch window.  The walker must
be racing with the PTE store itself — seeing a partially-written or
stale entry before `publishTranslationSlot` makes the invalid entry
visible.

Test: `20260317-011110`, 141 pass, 2 RAS.

### E6: Zero PTE instead of kernel vspace address (kernel commit e42e21e63)

Changed `pte_pte_invalid_new()` to write 0 instead of
`addrFromPPtr(armKSGlobalUserVSpace) & mask`.

Result: **no effect**. First run: 0 RAS (lucky). Second run: 2 RAS.
Same non-deterministic pattern.  The walker is not following the
invalid PTE's address bits.

Tests: `20260317-011644` (0 RAS), `20260317-012020` (2 RAS).

## Status

All targeted fixes attempted.  The remaining 1-2 RAS errors per run
during CANCEL_BADGED_SENDS_0002 persist despite:
- ISB after TLBI (E1)
- Full TLBI at PT finalize (E2)
- VMALLS12E1IS after IPAS2E1 (E3) — architecturally correct, keeps
- Disable stage-2 during VTTBR switch (E5)
- Zero invalid PTE (E6)

The walker race is fundamentally about the speculative walker reading
a valid PTE from the page table **before** the retirement store
becomes visible.  The old valid entry is already in the walker's
pipeline.  No barrier or TLBI can recall an already-issued walk.

The remaining error may require:
- Accepting it as a hardware limitation on Cortex-A78AE
- ATF-level RAS handler that suppresses these known-benign errors
- Or a fundamentally different retirement approach (e.g. two-pass:
  first clear all PTEs + TLBI, then free memory, with a full DSB+ISB
  between passes to drain all in-flight walks)

`pte_pte_invalid_new()` writes `addrFromPPtr(armKSGlobalUserVSpace) &
0xfffffffff000` — non-zero address bits with bits[1:0]=0.  If the
walker reads a partially-updated PTE, the non-zero address bits might
be followed speculatively.  Try writing 0 instead.

### E3: VTTBR manipulation window — pending

The EL2 TLBI helpers temporarily change VTTBR_EL2 to set the target
VMID, then restore it.  If the walker issues a stage-2 walk during
this VMID-switch window, it might use the wrong stage-2 tables.

### E4: sel4test teardown pattern — confirmed

The RAS errors occur **exclusively** during `CANCEL_BADGED_SENDS_0002`
("cancelBadgedSends deletes caps").  Confirmed across all test runs
with runtime RAS (20260316-192501, 20260316-211209, 20260316-220306,
20260316-221053, 20260317-002640).

`cancelBadgedSends` does bulk cap revocation: walks the CNode and
deletes all badged caps.  When frame caps are deleted, `unmapPage` is
called for each.  When PT caps are deleted, `unmapPageTable` is called.
This produces a rapid-fire sequence of PTE retirement + TLBI in a tight
loop — hundreds of PTEs invalidated in rapid succession.

This is the highest-throughput vspace teardown path in sel4test.  The
speculative walker has many more chances to catch a stale entry because:
- Many PTEs being retired concurrently at different table levels
- The walker may prefetch entries from a PT that is about to be freed
- The freed PT memory may be reused (zeroed or retyped) while the
  walker still has a stale reference to it in its pipeline

The fix likely needs to address the bulk teardown path (e.g. defer
PT freeing until all leaf entries are retired and TLBI'd), not just
individual PTE barrier ordering.
