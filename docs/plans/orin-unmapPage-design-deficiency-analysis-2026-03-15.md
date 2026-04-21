# Independent Analysis: `unmapPage()` Structural Design Deficiency

Date: 2026-03-15

## Context

This document records an independent analysis of whether seL4's `unmapPage()`
is faulty by design, prompted by a separate ChatGPT analysis that arrived at
that conclusion. The analysis is performed against current kernel source in this
workspace and the companion audit memo
[orin-arm64-el2-ras-audit-memo-2026-03-15.md](/home/hlyytine/tii-sel4/docs/plans/orin-arm64-el2-ras-audit-memo-2026-03-15.md).

## Verdict

**Yes. `unmapPage()` is structurally deficient by design.**

The deficiency is not a missing line of code or a single forgotten barrier. It
is an architectural constraint imposed by the frame-cap metadata schema that
forces `unmapPage()` into a trust-the-live-tree-walk pattern that is
fundamentally unsafe when combined with seL4's weaker-than-necessary
translation-object lifecycle guarantees on hardware with complex cache
hierarchies like Orin.

## The Core Design Problem

The original `unmapPage()` in
[vspace.c:1292](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1292)
does this:

1. `findVSpaceForASID(asid)` -- resolve ASID to vspace root
2. `lookupPTSlot(vspace_root, vptr)` -- **walk the live translation tree** to
   find the leaf slot
3. Read the live PTE word from that slot
4. Check it is a page type and the physical base matches
5. Invalidate the slot and TLBI

**The fundamental problem is step 2.** The kernel must re-walk a live, mutable
translation tree to find the slot it wants to retire.

This is forced by the frame cap design in
[structures.bf:23-33](/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/64/mode/object/structures.bf#L23).
Frame caps store:

| Field               | Bits | Purpose                    |
|---------------------|------|----------------------------|
| `capFMappedASID`    | 16   | ASID of the mapping        |
| `capFBasePtr`       | 48   | Physical frame base        |
| `capFSize`          | 2    | Page size                  |
| `capFMappedAddress` | 48   | Virtual address of mapping |
| `capFVMRights`      | 2    | Access rights              |
| `capFIsDevice`      | 1    | Device memory flag         |

They do **not** store:

1. A direct pointer to the parent page-table slot
2. The parent page-table object identity
3. Any reverse-map token that would allow direct slot retirement without a fresh
   walk

So `unmapPage()` has no choice but to rediscover the slot through a live walk of
the translation tree. This is the root structural deficiency.

## Why The Live Walk Is Unsafe

The `lookupPTSlot()` walker at
[vspace.c:664](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L664)
is a trusting reader:

```c
while (pte_pte_table_ptr_get_present(ret.ptSlot) && likely(level > 0)) {
    level--;
    ret.ptBitsLeft -= PT_INDEX_BITS;
    pt = paddr_to_pptr(pte_pte_table_ptr_get_pt_base_address(ret.ptSlot));
    ret.ptSlot = pt + ((vptr >> ret.ptBitsLeft) & MASK(PT_INDEX_BITS));
}
```

At each level the walker:

1. Reads a raw 64-bit PTE word from memory
2. Checks bits `[1:0]` and bit 58 to classify it as a table descriptor
3. **Extracts the physical base address and trusts it completely**
4. Converts it to a kernel pointer with `paddr_to_pptr()`
5. Dereferences it as a pointer to the next level

If that PTE word is stale or corrupt -- which the Orin RAS errors demonstrate is
happening -- the walker follows a garbage pointer. The kernel has no secondary
source of truth to validate against.

The follow-on helpers then compound the trust assumption:

1. `pte_pte_table_ptr_get_pt_base_address()` at
   [structures_gen.h:2291](/home/hlyytine/tii-sel4/orinagx_sel4test/kernel/generated/arch/object/structures_gen.h#L2291)
   asserts the slot still has tag `pte_pte_table`
2. `pte_get_page_base_address()` at
   [structures.h:222](/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/64/mode/object/structures.h#L222)
   assumes the loaded word is still a valid leaf descriptor

Once any of these trust assumptions is violated by stale data, Orin's
interconnect reports an ACI-side error before the kernel can recover.

## The Three-Contract Incoherence

One translation-object lifecycle currently crosses three inconsistent cache
maintenance contracts:

### 1. Reset/zeroing: no cache maintenance

[resetUntypedCap()](/home/hlyytine/tii-sel4/kernel/src/object/untyped.c#L234)
uses [clearMemory()](/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/machine.h#L47),
which is only `memzero()` with no cache operations.

### 2. Object creation: PoU publication (`dc cvau`)

[Arch_createObject()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c#L448)
publishes with `cleanCacheRange_PoU()`, which uses `dc cvau`.

### 3. Explicit PT unmap: PoC retire (`dc civac`)

[clearMemory_PT()](/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/machine.h#L68)
adds `cleanInvalidateCacheRange_RAM()`, which uses `dc civac`.

This means a reused page-table object may have been:

- Zeroed with no cache maintenance at all
- Published with PoU-only maintenance
- While the hardware walker or a later software walker can still observe
  stale PoC-level data from the previous incarnation

On Orin, which has system-level caches (L3, SLC) that sit beyond PoU and only
respect PoC operations, this is a real coherency gap.

## Missing Disciplines vs Linux/KVM Analogues

Linux arm64/KVM demonstrates the stronger protocol that seL4 lacks:

### Publication visibility

Linux
[mmu.c:400](/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/mm/mmu.c#L400)
explicitly does `dsb(ishst)` after allocating a zeroed page-table page, with
the comment: *"Ensure the zeroed page is visible to the page table walker."*

seL4 does not have an equivalent walker-visibility step between zeroing and
parent-link publication.

### Break-before-make discipline

KVM stage-2 replacement/unmap paths in
[kvm/hyp/pgtable.c](/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/kvm/hyp/pgtable.c#L627)
clear valid entries first, perform BBM-style TLB maintenance, then reinstall or
free.

seL4 does plain stores followed by cache maintenance.

### One-copy publication stores

Linux uses `WRITE_ONCE()` and `smp_store_release()` for descriptor publication.

seL4 uses ordinary C stores: `*slot = pte`.

### Ownership / refcounting

KVM carries explicit ownership/refcount structure for live vs invalid-but-owned
entries and child tables.

seL4 has no ownership-bearing intermediate invalid state. A single
`capPTIsMapped` bit is a tree-link property, not a dependency-liveness property.

## Why Barrier Tweaks Cannot Fix This

The experimental record from the audit memo demonstrates that no amount of
barrier/TLBI adjustments eliminates the problem:

| Experiment | Change                                              | Result                          |
|------------|-----------------------------------------------------|---------------------------------|
| K4/K5      | Live-path publication/retirement tightening         | Changed signature, did not fix  |
| K6         | EL2 TLBI completion tweak                           | No meaningful improvement       |
| K7         | Full-ASID leaf retirement instead of VA-targeted    | Shifted back to older signature |
| D2         | Direct leaf-slot identity in frame caps             | Shifted to new ACE AW Decode    |
| D3         | Slot-identity frame retirement + PT liveness assert | Exposed PT liveness accounting  |

Every experiment shifted the runtime failure signature without eliminating it.
This is the hallmark of a structural issue, not a missing fence.

## Why `unmapPage()` Itself Is Not "Faulty" In Isolation

To be precise: `unmapPage()` does exactly what it can given the cap metadata it
receives. The function is correctly implemented against the API it is given.

The real design deficiency is the **frame-cap schema** that forces `unmapPage()`
into a mandatory re-walk trust pattern, combined with **insufficient
cache/barrier discipline across the whole translation-object lifecycle** for
hardware with complex cache hierarchies like Orin.

`unmapPage()` is the most visible symptom because:

1. It is the dominant teardown-side reader of live translation state
2. Both explicit page unmap (`performPageInvocationUnmap`) and final-cap
   deletion (`Arch_finaliseCap`) converge on it
3. The historically dominant trigger tests (`FPU0001`,
   `CANCEL_BADGED_SENDS_0002`) are teardown-heavy workloads

But the root cause sits one level deeper in the metadata design.

## What A Fix Requires

Based on the experimental record and source analysis, the minimum viable fix
requires structural changes, not barrier adjustments:

### 1. Frame caps must store direct leaf-slot identity

Replace `capFMappedAddress` (exact virtual address) with:

- Parent PT base page number (36 bits on 40-bit VA config)
- Leaf slot index (9 bits)

This allows `unmapPage()` to retire a leaf by direct slot identity instead of
rediscovering it through a live tree walk.

### 2. Parent-PT liveness tracking must exist

A per-PT live-leaf reference count (or pin) in side metadata keyed by PT base
pointer, so that:

- PT reuse is blocked while mapped frame caps still reference slots within it
- `unmapPage()` can trust the stored slot identity because the parent PT is
  guaranteed to still be meaningful

### 3. Publication/retirement must use stronger one-copy discipline

- Publication: explicit walker-visibility step (`dsb(ishst)` or equivalent)
  between zeroing child pages and publishing the parent descriptor
- Retirement: clear-first, ensure walker-visible, then TLBI, then allow reuse
- Stores: use explicit one-copy semantics, not anonymous C `*slot = pte`

### 4. Reuse must be gated on retirement completion

Generic `resetUntypedCap()` must not allow translation-object reuse until both
tree-link state and live-leaf liveness state confirm the object is dead.

## Relationship To The D-Series Redesign

The audit memo's D-series experiments (D1: PT liveness scaffolding, D2: slot
identity in frame caps, D3: identity-based retirement) are exactly the right
shape of fix. The fact that D3 exposed a real PT-liveness accounting bug during
`Arch_finaliseCap()` confirms the redesign is on the correct path -- the
bookkeeping is incomplete but the structural direction is sound.

## Resolution (2026-03-16)

The deficiency has been fixed on branch `frame-cap-slot-identity` with two
commits:

1. **Frame cap slot identity redesign**: Frame caps now store direct
   leaf-slot identity (packed PT base PFN + slot index) instead of the
   mapped virtual address. `unmapPageBySlotID()` retires the leaf directly
   without any tree walk. ARM architectural violations (BBM, walker
   visibility, volatile stores, TLBI completion barriers, `clearMemory`
   cache maintenance) were fixed as part of this restructuring.

2. **Page table cap parent slot identity**: PT caps now store the parent
   slot identity (30 bits in existing padding). When PT finalization runs
   after the ASID has been deleted (common during bulk cap revocation),
   the parent table descriptor is directly invalidated using the stored
   identity. This fixed the last residual RAS errors in
   `CANCEL_BADGED_SENDS_0002`.

Validation: Orin AGX sel4test — 141 tests passed, 0 RAS errors.

A separate, non-deterministic RAS issue in the UEFI/ATF elfloader handoff
phase remains and is being investigated separately.

## Summary

| Question                                           | Answer |
|----------------------------------------------------|--------|
| Is `unmapPage()` faulty by design?                 | Yes    |
| Is the function itself incorrectly implemented?    | No     |
| Is the fault a missing barrier?                    | No     |
| Is the fault in the cap metadata schema?           | Yes    |
| Can barrier tweaks fix it?                         | No     |
| Does the experimental record confirm this?         | Yes    |
| Is the D-series redesign the right shape of fix?   | Yes    |
| **Has the deficiency been fixed?**                 | **Yes** |
