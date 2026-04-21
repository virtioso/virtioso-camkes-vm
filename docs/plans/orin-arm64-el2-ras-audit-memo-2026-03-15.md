# Orin AArch64 EL2 RAS Audit Memo

## Scope

This memo summarizes the current source-only audit state for recurring Orin AGX
RAS errors in seL4 AArch64 hypervisor mode.

It is derived from:

1. [orin-arm64-el2-ras-source-audit-plan-2026-03-15.md](/home/hlyytine/tii-sel4/docs/plans/orin-arm64-el2-ras-source-audit-plan-2026-03-15.md)
2. current seL4 kernel source
3. current libsel4utils/libsel4vspace/libsel4allocman source
4. existing local Orin investigation notes

This memo now also incorporates two post-implementation runtime validation
runs:

1. Autopilot request `20260315-191401`
2. image:
   [sel4test-driver-image-arm-orinagx](/home/hlyytine/tii-sel4/orinagx_sel4test/images/sel4test-driver-image-arm-orinagx)
3. result:
   - run canceled after clear reproduction of the old SCC/ACI RAS family
   - surviving `ELR_EL3` values still resolve to:
     - `pte_ptr_get_pte_type()` lines 2272/2273
     - `unmapPage()` line 1071
4. Autopilot request `20260315-194544`
5. image:
   [sel4test-driver-image-arm-orinagx](/home/hlyytine/tii-sel4/orinagx_sel4test/images/sel4test-driver-image-arm-orinagx)
6. result:
   - run canceled after a materially changed but still failing RAS pattern
   - dominant sampled `ELR_EL3` values no longer pointed at
     `unmapPage()` / `pte_ptr_get_pte_type()`
   - new dominant sampled `ELR_EL3` values resolved into:
     - `lookupPTSlot()`
     - `pte_pte_table_ptr_get_pt_base_address()`
     - `pte_get_page_base_address()`
   - sampled output showed a long `ACI`-only flood rather than the earlier
     `SCC` + `ACI` pair
7. Autopilot request `20260315-195906`
8. image:
   [sel4test-driver-image-arm-orinagx](/home/hlyytine/tii-sel4/orinagx_sel4test/images/sel4test-driver-image-arm-orinagx)
9. result:
   - run canceled after clear reproduction of the same post-`K4`/`K5` failure
     shape
   - sampled output again showed a persistent `ACI`-only flood
   - sampled `ELR_EL3` values again clustered in the same walk/decode family
   - the narrow `K6` EL2-TLBI completion tweak did not produce a meaningful
     improvement over `V3`
10. Autopilot request `20260315-204025`
11. image:
    [sel4test-driver-image-arm-orinagx](/home/hlyytine/tii-sel4/orinagx_sel4test/images/sel4test-driver-image-arm-orinagx)
12. result:
    - run canceled after the experimental `K7` failure shape was clear
    - the full-ASID leaf-retirement experiment did not remove the failure
    - the runtime signature snapped back toward the older mixed `SCC` + `ACI`
      family
    - repeated `ELR_EL3` values again resolved to:
      - `pte_ptr_get_pte_type()` line 2272
      - `pte_ptr_get_pte_type()` line 2273
    - repeated fault addresses again clustered near the old `0x7ffffxxx`
      family
13. Autopilot request `20260315-212019`
14. image:
    [sel4test-driver-image-arm-orinagx](/home/hlyytine/tii-sel4/orinagx_sel4test/images/sel4test-driver-image-arm-orinagx)
15. result:
    - run canceled after the `D2` failure shape was clear
    - the failure persists after switching runtime frame caps to direct
      leaf-slot identity
    - sampled output showed a pure `ACI` flood, without the older `SCC` pair
    - decoded RAS tuple shifted to:
      - `IERR = ACE AW Decode Error: 0xc`
      - `SERR = Assertion failure: 0x4`
    - reported addresses shifted to a new high-address family such as
      `0x8000b771d6df5df0`
    - hot `ELR_EL3` values stayed in relocated high form
      (`0x8273bf408`, `0x8273bfa20`, `0x8273bfa40`, `0x8273bfa4c`) and did not
      resolve directly with `addr2line`
16. Autopilot request `20260315-213640`
17. image:
    [sel4test-driver-image-arm-orinagx](/home/hlyytine/tii-sel4/orinagx_sel4test/images/sel4test-driver-image-arm-orinagx)
18. result:
    - extended wait reached seL4 and reproduced the older mixed `SCC` + `ACI`
      low-address family again
    - sampled `ELR_EL3` moved to `0x80800234b4`
    - reported `ADDR` was `0x800000007fffffc0`
    - the kernel then halted on a new `D3` assertion:
      `armPTLivenessGet(PTE_PTR(cap_page_table_cap_get_capPTBasePtr(cap))) == 0`
      in `Arch_finaliseCap()`
19. Autopilot requests `20260315-214500` and `20260315-215126`
20. image:
    [sel4test-driver-image-arm-orinagx](/home/hlyytine/tii-sel4/orinagx_sel4test/images/sel4test-driver-image-arm-orinagx)
21. result:
    - the `D3` follow-up commit `d29ae0338` removed the old PT-liveness
      assertion
    - but both runs failed earlier, before seL4 reached `Bootstrapping kernel`
    - the failure occurs immediately after:
      `Enabling hypervisor MMU and jumping to entry point...`
    - firmware prints:
      `Synchronous Exception at 0x0000000819A83948`
    - that address is not a random UEFI code PC:
      - NVIDIA UEFI's AArch64 exception stub passes `ELR_EL1/ELR_EL2` into the
        printout
      - `0x819A83948 - 0x819A81000 = 0x2948`
      - `0x2948` resolves in the current elfloader image to
        `flush_dcache_range()` at
        [mmu.S](/home/hlyytine/tii-sel4/tools/seL4/elfloader-tool/src/arch-arm/armv/armv8-a/64/mmu.S#L84),
        specifically the `dc civac, x0` instruction
    - so the regression is an early handoff crash in the elfloader/firmware
      cache-maintenance path, not later seL4 PT-liveness code executing
      directly

## Executive Summary

The best current source-only reading is now narrower again.

The open Orin RAS family still looks like a translation-metadata
lifecycle/publication problem rather than a single isolated EL2 barrier
omission, but the post-implementation validation runs show that the dominant
surviving risk is in live mutation/lookup/TLBI paths, not in the
creation/final-delete paths that were fixed first.

The dominant observed detector family has shifted:

1. before `K4`/`K5`:
   - `unmapPage() -> lookupPTSlot() -> pte_ptr_get_pte_type()`
2. after `K4`/`K5`:
   - `lookupPTSlot()`
   - `pte_pte_table_ptr_get_pt_base_address()`
   - `pte_get_page_base_address()`
3. after the `K7` full-ASID leaf-retirement experiment:
   - the signature shifted back toward
     `pte_ptr_get_pte_type()` plus mixed `SCC` + `ACI` reports
4. after `D2` direct leaf-slot identity:
   - the signature shifted again into a pure `ACI` flood with `ACE AW Decode
     Error`
   - the observed bad-address family moved away from the older `0x7fff...`
     cluster
5. after `D3` direct slot-identity frame retirement:
   - the runtime signature shifts back toward the older low-address
     `SCC` + `ACI` family
   - the kernel now exposes a separate PT-liveness accounting bug during
     page-table finalisation
6. after the `D3` follow-up commit `d29ae0338`:
   - the PT-liveness assertion is gone
   - but the build regresses earlier into a real exception at the elfloader's
     `flush_dcache_range()` cache-maintenance instruction during handoff
   - this means the regression is indirect and payload-sensitive; it is not the
     PT-liveness logic running and crashing in the later seL4 path

The strongest current origin-side suspicion is now split:

1. live single-slot translation updates are still published/retired under a
   weaker contract than stronger upstream arm64/KVM analogues use
2. reset/reissue of translation objects remains a real secondary concern, but
   it no longer fits the surviving runtime detector path as cleanly as before

## Ranked Conclusions

### 1. Most plausible overall explanation

Lifecycle / reuse / publication weakness across multiple helpers is the
strongest explanation class.

This now fits both:

1. the historical sel4test trigger clustering
2. the current source-level asymmetries in reset, publication, teardown, and
   later reuse

Post-implementation runtime validation tightens this further:

1. the just-landed creation/final-delete sanitisation changes did not remove
   the dominant RAS family
2. so the leading untouched risk is now the live update path around:
   - `unmapPage()`
   - `lookupPTSlot()`
   - `pte_ptr_get_pte_type()`
   - single-address TLBI under EL2

The second validation run sharpens the weighting again:

1. the `K4`/`K5` live-path changes were not no-ops; they changed the runtime
   signature materially
2. but they did not remove the failure
3. the leading remaining risk is still the live walk/decode path plus EL2
   single-address TLBI interaction, but `K6` shows that a narrow helper-local
   completion tweak is not enough by itself

### 2. Best detector-path ranking

Current weighting from source plus the two validation runs is:

1. current primary runtime family:
   - `lookupPTSlot()`
   - `pte_pte_table_ptr_get_pt_base_address()`
   - `pte_get_page_base_address()`
2. older primary family that `K4`/`K5` displaced but did not eliminate at the
   root-cause level:
   - `unmapPage() -> lookupPTSlot() -> pte_ptr_get_pte_type()`
3. secondary: late PT/PD final-cap teardown that can feed `unmapPageTable()`
4. weaker still: final root-cap teardown that can feed `deleteASID()`
5. detector-style readers, not likely origins:
   - `findMapForASID()`
   - `isFinalCapability()`

### 3. Best workload-fit model

For the historically dominant trigger tests, especially `FPU0001` and
`CANCEL_BADGED_SENDS_0002`, the best fit is:

1. shared-vspace leaf-page churn inside one long-lived root
2. immediate reuse pressure on a split-backed freeable untyped allocator
3. delayed teardown detection later in one or more destruction/finalisation
   phases

Important split:

1. `FPU0001` fits immediate helper-thread stack / IPC-buffer frame churn
2. `CANCEL_BADGED_SENDS_0002` fits delayed teardown through
   `cteRevoke()` / `cteDelete()` / `Arch_finaliseCap()` and later
   process-destruction cleanup

## Strongest Origin-Side Model

The strongest current origin-side model is the reset/reissue boundary for
translation objects, but this must now be read as a secondary-origin model
rather than the sole leading one.

The relevant source-backed asymmetry is:

1. [resetUntypedCap()](/home/hlyytine/tii-sel4/kernel/src/object/untyped.c#L234)
   uses plain `clearMemory()`
2. [clearMemory()](/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/machine.h#L47)
   is only `memzero()`
3. reissued `VSpace` / `PageTable` objects in
   [Arch_createObject()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c#L448)
   are published with `cleanCacheRange_PoU()`
4. [cleanCacheRange_PoU()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/machine/cache.c#L98)
   uses `dc cvau`
5. only the explicit invocation-driven page-table unmap path
   [performPageTableInvocationUnmap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1192)
   applies [clearMemory_PT()](/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/machine.h#L68)
6. [clearMemory_PT()](/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/machine.h#L68)
   adds `cleanInvalidateCacheRange_RAM()`
7. on AArch64,
   [cleanInvalidateCacheRange_RAM()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/machine/cache.c#L40)
   uses `dc civac`

So one translation-object lifecycle is currently crossing three different
contracts:

1. bare zeroing
2. `PoU` publication (`dc cvau`)
3. `PoC`-style retire/clean-invalidate (`dc civac`)

That is the strongest current source-backed reason to suspect stale
translation state is introduced before the later detector windows.

After the failed runtime validation run, the stronger leading source-side
concern is:

1. live leaf unmap and live parent/leaf PTE publication paths still use plain
   stores followed by `cleanInvalByVA()` and TLBI
2. those untouched paths now fit the surviving detector PCs better than the
   creation/final-delete code that was changed
3. the highest-priority untouched sites are:
   - [unmapPage()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1040)
   - [performPageTableInvocationMap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1180)
   - [performPageInvocationMap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1203)

After the second failed validation run, this narrows further:

1. [performPageTableInvocationMap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1180)
   and [performPageInvocationMap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1203)
   were changed and did affect the runtime signature
2. the highest-priority untouched suspect is therefore the EL2
   single-address TLBI path behind
   [invalidateTLBByASIDVA()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L986)
   and
   [invalidateLocalTLB_IPA_VMID()](/home/hlyytine/tii-sel4/kernel/include/arch/arm/armv/armv8-a/64/armv/tlb.h#L36)
3. the remaining runtime issue now looks more like walk/decode while retiring
   translations than like a simple direct-overwrite publication bug
4. the failed `K6` validation weakens the case for more tiny EL2-TLBI barrier
   edits as the main path forward

Local Linux arm64 analogue that strengthens this concern:

1. [mmu.c](/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/mm/mmu.c#L400)
   explicitly does `dsb(ishst)` after allocating a zeroed page-table page,
   with the comment "Ensure the zeroed page is visible to the page table
   walker"
2. that does not by itself prove seL4 architecturally wrong
3. but it is a strong local upstream analogue that treats fresh page-table
   publication as requiring an explicit walker-visibility step, not just bare
   zeroing plus later parent-link publication

### Surviving Walk/Decode Invariant Chain

The deeper source-only read after `V3` and `V4` is that the remaining issue is
best described as a broken invariant chain inside the software walker itself,
not just as “some TLBI is wrong”.

The critical sequence is
[lookupPTSlot()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L664):

1. load one 64-bit slot word
2. classify it as a table descriptor by interpreting:
   - bits `[1:0]`
   - bit `58`
3. if it still looks like `pte_pte_table`, trust the embedded base address
4. convert that physical base into a kernel pointer with `paddr_to_pptr()`
5. continue walking the child table

The follow-on helpers then assume the same loaded word is still self-consistent:

1. [pte_pte_table_ptr_get_pt_base_address()](/home/hlyytine/tii-sel4/orinagx_sel4test/kernel/generated/arch/object/structures_gen.h#L2291)
   asserts the slot still has tag `pte_pte_table`
2. [pte_get_page_base_address()](/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/64/mode/object/structures.h#L222)
   assumes the loaded word is still a valid leaf descriptor

The post-`K4`/`K5` runtime PCs moving into `lookupPTSlot()`,
`pte_pte_table_ptr_get_pt_base_address()`, and `pte_get_page_base_address()`
mean the surviving failure happens while seL4 is still trusting one of these
two descriptor classes:

1. “this slot is still a table descriptor, so its base pointer is safe to
   follow”
2. “this slot is still a leaf descriptor, so its frame base is safe to use”

That narrows the likely bad states:

1. a stale but internally consistent old table descriptor is still being
   observed after software intended retirement
2. a stale but internally consistent old leaf descriptor is still being
   observed after software intended retirement
3. a descriptor-class confusion exists at the visibility level, not merely as a
   logical cap-state error

It weakens some earlier explanations:

1. this is not well described anymore as just “direct overwrite in
   `performPageInvocationMap()`”
2. it is also not well described as just “creation-time PoU publication”
3. and it is not explained well by `findMapForASID()` / `isFinalCapability()`
   style metadata readers

The stronger current source-only interpretation is:

1. software is still able to observe a slot as a valid table/leaf descriptor at
   the point it walks or decodes it
2. once that happens, the derived physical base is trusted quickly enough that
   Orin reports an ACI-side error before the kernel can recover
3. the remaining bug therefore sits in the retirement/observation contract for
   live table/leaf descriptors, not only in higher-level object lifecycle
   bookkeeping

### Active `lookupPTSlot()` Reader Contexts Under AArch64 EL2

The remaining question is which software readers can still hit that broken
invariant chain during live transitions.

Current direct callers in
[vspace.c](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c)
are:

1. [unmapPage()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1069)
   - transition-sensitive
   - walks the vspace, expects a leaf of the requested size, checks the frame
     base, then retires the slot
   - this is the strongest teardown-side reader
2. [decodeARMPageTableInvocation()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1493)
   - map-time `DeleteFirst` check
   - transition-sensitive, but only rejects if the destination slot already
     looks valid
   - weaker workload fit for the observed Orin floods
3. [decodeARMFrameInvocation()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1612)
   - map/remap path
   - transition-sensitive because it resolves the target slot before calling
     `performPageInvocationMap()`
4. [decodeARMVSpaceFlushInvocation()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1390)
   - vspace flush path
   - checks that the slot is a leaf and derives the physical start address
   - still a real walk/decode reader, but less likely than `unmapPage()` in the
     dominant teardown-heavy workload
5. `ARMPage*Flush` verification in
   [decodeARMFrameInvocation()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1693)
   - compiled out under `CONFIG_ARM_HYPERVISOR_SUPPORT`
   - not relevant for the current Orin EL2 case
6. [readWordFromVSpace()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1915)
   - debug/printing reader
   - can trust and follow leaf bases, but is a poor fit for the recurring test
     workload

This classification narrows the live reader set materially for Orin EL2:

1. the dominant transition-sensitive readers are the unmap path and the
   map/remap `lookupPTSlot()` checks
2. the EL1-only page-flush verifier is not in play here
3. the recurring runtime issue therefore does not need a large number of reader
   contexts to explain it; a small walk/decode family is enough

That strengthens the current interpretation:

1. the surviving bug is concentrated in the contract between live descriptor
   retirement/publication and the small set of readers that still trust table
   and leaf tags
2. `unmapPage()` remains the best workload-fit teardown reader
3. the map/remap-side `lookupPTSlot()` callers remain plausible secondary
   observation points if a slot stays self-consistent long enough to pass their
   validity checks

### Reader Dominance In The Current Workload

The call graph and workload fit now make the dominance ordering clearer.

Highest-frequency path:

1. helper-thread and teardown-heavy userspace flows issue
   `seL4_ARM_Page_Unmap`
2. kernel-side that goes through
   [performPageInvocationUnmap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1235)
3. final-cap deletion also feeds the same reader through
   [Arch_finaliseCap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c#L190)
4. both converge on
   [unmapPage()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1069)
5. `unmapPage()` is therefore still the strongest workload-fit live reader of
   the surviving descriptor-trust chain

Lower-frequency secondary readers:

1. page-table map-time `DeleteFirst` check in
   [decodeARMPageTableInvocation()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1493)
2. frame map/remap lookup in
   [decodeARMFrameInvocation()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1612)
3. vspace flush lookup in
   [decodeARMVSpaceFlushInvocation()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1390)

Why they rank lower:

1. they require explicit map/remap/flush invocation paths
2. the dominant historical trigger tests are teardown-heavy, not map-heavy
3. `CANCEL_BADGED_SENDS_0002` reaches arch teardown through generic cap
   deletion/finalisation, which again feeds `unmapPage()`
4. `FPU0001` helper cleanup also feeds repeated page unmap directly

Current reader ranking:

1. dominant: `unmapPage()`
2. plausible secondary: map/remap `lookupPTSlot()` checks
3. weaker: vspace-flush lookup
4. weak debug path: `readWordFromVSpace()`

This matters because it narrows the most likely surviving observation site:

1. the remaining Orin issue probably does not need a general software-walker
   failure across all reader contexts
2. a failure concentrated on teardown-side `unmapPage()` reads is enough to fit
   both the source and the old sel4test trigger clustering

## Teardown Model

The staged-teardown reading is now explicit in source.

### Kernel side

Mapped frame/page-table/vspace caps reach arch teardown through
[Arch_finaliseCap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c#L146):

1. frame cap -> `unmapPage()`
2. mapped page-table cap -> `unmapPageTable()`
3. mapped vspace cap -> `deleteASID()`

But generic final-cap deletion retires reachability more consistently than it
sanitizes underlying translation memory.

### Userspace process-destruction side

[sel4utils_destroy_process()](/home/hlyytine/tii-sel4/projects/seL4_libs/libsel4utils/src/process.c#L674)
does:

1. thread cleanup
2. `vspace_tear_down()`
3. `clear_objects()`
4. final root-object free

[vspace_tear_down()](/home/hlyytine/tii-sel4/projects/seL4_libs/libsel4utils/src/vspace/vspace.c#L949)
walks leaf mappings first.

The later [clear_objects()](/home/hlyytine/tii-sel4/projects/seL4_libs/libsel4utils/src/process.c#L53)
phase then frees recorded paging objects.

Those recorded objects are not generic:

1. they come from `seL4_FailedLookup` paging-object allocation paths
2. on the active Orin AArch64+EL2+40-bit config, they are specifically:
   - `seL4_ARM_PageTableObject`
   - `seL4_ARM_PageDirectoryObject`

So one process-destruction path naturally contains multiple delayed detector
windows:

1. leaf teardown first
2. PT/PD final-cap teardown later
3. root-cap teardown weaker still

## What This Means For `eba5a3b6...`

Current verdict on `eba5a3b6e6b804a77c6cfc8d2a6535707c953c9a`:

1. keep the code for now
2. rewrite the rationale
3. treat it as partial

Why:

1. switching runtime mutation-side maintenance toward conservative `PoC`
   behavior is directionally useful on Orin-class systems
2. but the commit message overstates the claim if it says the walker simply
   reads from `PoC`
3. and the commit does not solve the stronger current issues:
   - live leaf unmap / lookup / TLBI behavior
   - live parent/leaf PTE publication behavior
   - generic reset with plain `clearMemory()`
   - staged teardown and later reuse

Safer rationale:

1. `PoC` is a conservative implementation choice for Orin-class systems and
   mixed-cache hierarchies
2. this change cleans up one mutation-side visibility dimension
3. it is not the full coherency/lifecycle fix

## What Looks Weaker Now

These explanations are currently weaker than the full lifecycle/live-path model:

1. one single missing EL2 barrier as the dominant cause
2. `findMapForASID()` as an origin site
3. `isFinalCapability()` as an origin site
4. reset/reissue alone as the full explanation after the failed runtime
   validation
5. valid-to-valid remap ordering in `performPageInvocationMap()` as the main
   workload fit for the dominant trigger tests

The remap path still matters, especially for cacheability-changing remaps, but
it currently looks secondary to the reset/reuse/publication story.

## Structural Constraint In `unmapPage()`

The deeper source study now shows that the surviving issue is not just “a live
reader exists”. It is that the dominant live reader,
[unmapPage()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1063),
cannot avoid trusting the live walked descriptor with the current cap schema.

Current frame caps store only:

1. mapped ASID
2. mapped virtual address
3. frame base pointer
4. page size

See
[structures.bf](/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/64/mode/object/structures.bf#L23).

They do not store:

1. the parent page-table slot pointer
2. the parent page-table object identity
3. any reverse-map token that would let the kernel retire the mapping directly
   without a fresh walk

That forces the current `unmapPage()` structure:

1. [findVSpaceForASID()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L589)
2. [lookupPTSlot()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L664)
3. size check against `ptBitsLeft`
4. load the live slot word
5. classify it as a leaf with `pte_is_page_type()`
6. verify the live leaf base matches the cap’s frame base
7. only then retire the slot

This same obligation appears in both entry paths:

1. direct page unmap through
   [performPageInvocationUnmap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1235)
2. final-cap teardown through
   [Arch_finaliseCap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c#L190)

The stronger current conclusion is therefore:

1. with the present frame-cap API, `unmapPage()` cannot simply retire from cap
   state alone
2. it must re-walk and re-validate the live descriptor
3. so if Orin can still expose a stale but self-consistent table/leaf
   descriptor at that moment, `unmapPage()` is structurally forced to trust it
   far enough to trigger the surviving walk/decode-family failures

This makes the remaining problem more specific:

1. the issue is not just missing maintenance on the write side
2. it is also that the dominant teardown reader has no alternate source of
   slot identity once a frame cap is already mapped
3. avoiding this class completely would require either:
   - stronger live descriptor retirement/observation guarantees than seL4
     currently achieves on Orin, or
   - a different mapping metadata design that preserves direct slot identity
     across unmap/finalise paths

## Current Practical Direction

Because `K4`, `K5`, and `K6` are now implemented and still left the
walk/decode-family failure alive, the next high-value work is no longer
another small maintenance tweak. It is one of these two deeper directions:

1. study whether the live descriptor retirement/observation guarantee can be
   made stronger enough for `unmapPage()` to remain safe with the current cap
   schema
2. study whether AArch64 frame-cap mapping metadata would need a more direct
   slot-identity design to avoid mandatory re-walk trust at unmap time

At the current source-only stage, option 1 is the nearer-term investigation
target and option 2 is the architectural redesign fallback.

### What Option 1 Must Actually Prove

The first option is now clearer after checking the active kernel
configuration and the kernel entry/locking model.

On the active Orin build:

1. [CONFIG_MAX_NUM_NODES](/home/hlyytine/tii-sel4/orinagx_sel4test/kernel/gen_config/kernel/gen_config.h#L123)
   is `1`
2. `CONFIG_ENABLE_SMP_SUPPORT` is disabled in
   [gen_config.h](/home/hlyytine/tii-sel4/orinagx_sel4test/kernel/gen_config/kernel/gen_config.h#L124)
3. the SMP `NODE_LOCK` machinery in
   [lock.h](/home/hlyytine/tii-sel4/kernel/include/smp/lock.h#L107) therefore
   does not describe the active Orin execution model

That materially changes how the surviving reader/writer problem should be read:

1. the current Orin failures are not well explained as plain concurrent
   software reader/writer races between different CPUs
2. `unmapPage()` and other `lookupPTSlot()` readers may be separated in time
   from earlier writers by preemption and later teardown phases, but not by
   simultaneous SMP mutation of the same slot
3. so if a later serialized software reader still sees a stale but
   self-consistent descriptor, the stronger explanation is that the descriptor
   state has remained or become stale/corrupt across the live-retirement
   boundary, not that another core is racing the reader in real time

This makes the first option both narrower and harsher:

1. a stronger retirement/observation contract could still solve the problem
   with the current cap schema
2. but only if it guarantees that once software retires a live descriptor, no
   later serialized software walk can still observe a valid-looking stale
   table/leaf word at that slot
3. that is a much stronger requirement than “add one more fence around a live
   writer”

The `K7` experiment strengthens that conclusion:

1. replacing VA-targeted retirement with full-ASID retirement for the dominant
   `unmapPage()` leaf-unmap path did not eliminate the failure
2. instead, it pulled the runtime signature back toward the older
   `pte_ptr_get_pte_type()` plus mixed `SCC` + `ACI` family
3. so “stronger invalidate primitive on the existing live-retirement path” is
   not enough by itself

The local Linux/KVM analogue shows what this stronger discipline tends to look
like:

1. zeroed page-table pages are made walker-visible explicitly with `dsb(ishst)`
   in
   [mmu.c](/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/mm/mmu.c#L76)
   and
   [kvm/hyp/pgtable.c](/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/kvm/hyp/pgtable.c#L1136)
2. stage-2 replacement/unmap paths clear valid entries first and perform
   break-before-make style TLB maintenance before reinstalling or freeing in
   [kvm/hyp/pgtable.c](/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/kvm/hyp/pgtable.c#L627)
   and
   [kvm/hyp/pgtable.c](/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/kvm/hyp/pgtable.c#L890)
3. KVM also carries more explicit ownership/refcount structure for live versus
   invalid-but-owned entries than seL4 does

Compared to that, current seL4 still relies on a thinner live-slot contract:

1. one descriptor word
2. plain store plus cache maintenance
3. TLBI
4. later software re-walk and trust of that same slot

The source-only verdict for option 1 is therefore:

1. it remains the better near-term investigation path
2. but it probably requires a materially stronger live-retirement protocol, not
   another tiny helper-local barrier adjustment
3. if seL4 cannot prove that protocol strongly enough on Orin, then the backup
   conclusion will be that the current frame-cap schema leaves `unmapPage()`
   too dependent on re-trusting a live descriptor
4. the `K7` result weakens “try a stronger invalidate flavor” as a standalone
   sub-strategy inside option 1

### What A Stronger Live-Retirement Protocol Would Need

The next useful question is no longer “which single barrier is missing?” It is
“what protocol would be strong enough that a later serialized
`lookupPTSlot()`/`unmapPage()` reader cannot still trust a stale descriptor?”

From current seL4 source and the local Linux/KVM analogues, the minimum shape
looks like this:

1. publication side:
   - fully initialize child translation memory before publishing the parent
     descriptor that points to it
   - make that initialized child walker-visible before the parent publish
   - publish the parent/leaf descriptor with an explicit one-copy store
2. retirement side:
   - clear the live descriptor to invalid first
   - make that invalid word walker-visible before any later reuse/free of the
     child/frame it used to name
   - complete the required TLBI sequence before any later reader is allowed to
     depend on the slot having retired
3. ownership side:
   - preserve enough ownership information across the invalidated state that the
     kernel can distinguish “invalid and dead” from “invalid but still owned /
     awaiting post-retirement cleanup”
   - do not immediately collapse retirement and ownership loss into one bare
     invalid word if later teardown or reuse depends on knowing what used to
     live there
4. reuse side:
   - do not allow translation-object or frame reuse until the retirement state
     is fully completed, not merely logically requested

The local Linux/KVM code shows several pieces of this stronger model:

1. new table publication uses explicit walker-visibility steps such as
   `dsb(ishst)` before later hardware walk consumption
2. valid-entry replacement/unmap clears the old entry first and performs BBM-
   style TLB maintenance before reinstalling or freeing
3. KVM keeps more explicit ownership/refcount state for invalid-but-owned
   entries and child tables than seL4 does
4. publication uses explicit one-copy stores such as `WRITE_ONCE()` and
   `smp_store_release()`, rather than relying purely on an ordinary C store

Compared to that, current seL4 is still much thinner:

1. publish:
   - ordinary `*slot = pte`
   - `cleanInvalByVA()`
2. retire:
   - ordinary `*slot = invalid`
   - `cleanInvalByVA()`
   - TLBI
3. ownership after invalidation:
   - largely external to the slot itself
   - no invalid-but-owned descriptor state
4. later teardown reader:
   - must re-walk and trust the slot again

That suggests option 1 is only viable if it is interpreted as a bigger protocol
upgrade with at least these concrete objectives:

1. make slot publication/retirement explicit one-copy operations, not plain
   anonymous C stores
2. define and document when retirement is considered complete for later
   `lookupPTSlot()`-style readers
3. prevent premature reuse relative to that completion point
4. decide whether seL4 needs an ownership-bearing intermediate invalid state,
   or whether it can prove safety without one

The hardest remaining gap is ownership:

1. even if publication and TLBI sequencing are tightened, `unmapPage()` still
   has to re-trust a live slot because frame caps do not encode direct slot
   identity
2. if seL4 cannot preserve enough meaning across the invalidated state for
   later teardown/reuse, then protocol strengthening alone may still stop short
3. that is the boundary where option 1 would give way to the metadata-design
   work of option 2

### Can Current seL4 Reach That Protocol Without Changing Cap Metadata?

This is now the decisive source-only question. The answer is mixed, and it is
best stated per protocol area.

#### 1. Publication: probably yes

Current source can likely be lifted to a stronger publication protocol without
changing cap metadata.

Why:

1. translation-object creation already has a dedicated arch hook in
   [Arch_createObject()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c#L407)
2. live parent/leaf publication already funnels through
   [performPageTableInvocationMap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1208)
   and
   [performPageInvocationMap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1226)
3. those sites can be tightened further with more explicit one-copy publication
   discipline and stronger documented walker-visibility rules

Verdict:

1. publication alone does not force a metadata redesign

#### 2. Retirement: maybe, but only with a stronger completion definition

Current source can probably be tightened further on retirement without changing
cap metadata, but only if seL4 defines a much stronger notion of when
retirement is complete for later software readers.

Why:

1. live retirement already funnels through
   [unmapPage()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1063)
   and
   [unmapPageTable()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1040)
2. explicit final-cap translation-object sanitisation now exists in
   [Arch_finaliseCap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c#L146)
3. but the `K7` experiment showed that simply swapping the invalidate flavor on
   the current leaf-unmap path is not enough

Verdict:

1. retirement protocol tightening still looks possible
2. but it has to be broader than “which TLBI primitive is used”

#### 3. Ownership across invalidation: not convincingly

This is the weakest area for the current design and the main reason option 1
may stop short.

Why:

1. frame caps only preserve:
   - mapped ASID
   - mapped virtual address
   - frame base
   - size
2. they do not preserve direct slot identity or parent-table identity
3. slot format itself uses a single invalid descriptor state, not an
   “invalid-but-still-owned / retirement-pending” state
4. later teardown readers like
   [unmapPage()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1063)
   therefore have to re-walk and re-trust the live slot

Verdict:

1. current seL4 does not obviously have enough ownership structure to express a
   strong intermediate retirement state
2. this is the first area where a metadata-design change looks materially more
   plausible than further protocol tightening alone

#### 4. Reuse gating: only partially

Current source has some places where reuse can be tightened without metadata
changes, but the generic reset/retype path still weakens the story.

Why:

1. explicit PT finalisation already sanitises translation memory in
   [performPageTableInvocationUnmap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1214)
   and
   [Arch_finaliseCap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c#L158)
2. but generic untyped reset still goes through
   [resetUntypedCap()](/home/hlyytine/tii-sel4/kernel/src/object/untyped.c#L234),
   which fundamentally knows nothing about “translation retirement not fully
   completed yet”
3. later reissue depends on object-type-specific sanitisation at creation time,
   not on a generic retirement token carried through reset

Verdict:

1. reuse can be made safer
2. but the current generic reset path does not naturally encode a strong
   retirement-completion contract

### Net Verdict

The current cap schema probably can support a stronger protocol in two limited
areas:

1. publication
2. some retirement-side tightening

But it does not look sufficient to solve the whole problem cleanly in the two
hardest areas:

1. ownership across invalidation
2. reuse gating across generic reset/retype

So the current best source-only answer is:

1. option 1 is still worth pushing a bit further conceptually
2. but a full fix within the current metadata model is no longer the leading
   expectation
3. the most likely boundary where it breaks is that `unmapPage()` still has no
   trustworthy identity source except a fresh walk of the live slot
4. if future experiments keep failing after stronger publication/retirement
   tightening, option 2 should become the primary path rather than the backup

## Smallest Plausible Metadata Redesign

If option 2 becomes primary, the next question is what the smallest plausible
redesign actually is.

The answer is not “just store a few more bits”. The design space is narrower
than that.

### Candidate A: replace stored mapped `vaddr` with direct leaf-slot identity

This is the smallest cap-local redesign that looks technically plausible.

Current frame caps spend 48 bits on
`capFMappedAddress` in
[structures.bf](/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/64/mode/object/structures.bf#L23).

On the active Orin EL2 build:

1. `seL4_UserTop` is
   [0x000000ffffffffff](/home/hlyytine/tii-sel4/kernel/libsel4/sel4_arch_include/aarch64/sel4/sel4_arch/constants.h#L248)
   so user VAs are 40-bit
2. frame mappings are at least 4 KiB aligned
3. a parent PT base is 4 KiB aligned as well

That means a 48-bit cap field can plausibly encode:

1. parent PT base page number: 36 bits
2. leaf slot index: 9 bits
3. a few spare bits

instead of an exact mapped virtual address.

Why that is attractive:

1. `unmapPage()` could retire a leaf by direct slot identity instead of
   `findVSpaceForASID()` plus `lookupPTSlot()` over the live tree
2. final-cap frame teardown could use the same direct identity
3. map/remap-time “same mapping” checks could compare requested slot identity
   instead of exact stored `vaddr`

Why it is not sufficient by itself:

1. the current frame-cap API also uses stored `vaddr` for:
   - remap same-address checks
   - page flush relative-range operations
   - some boot/capDL reporting paths
2. those uses are reworkable on EL2, but not free
3. more importantly, direct parent-PT identity alone does not solve parent PT
   lifetime and reuse

### Candidate B: keep cap format, add reverse-map side metadata

This is the cleanest semantic redesign, but it is larger.

Idea:

1. keep frame caps storing `ASID + frame base + size + mapped vaddr`
2. add kernel-side mapping metadata that preserves direct slot identity and
   perhaps a generation/ownership token
3. let `unmapPage()` consume that trusted metadata instead of re-deriving
   identity only from a live walk

Why it is attractive:

1. preserves current cap semantics
2. gives room for richer retirement/ownership state than one cap field can hold

Why it is expensive:

1. cap copies must still refer to one shared mapping state
2. lifetime across revoke, final deletion, and untyped reset becomes a new
   kernel invariant
3. it is a larger change than the cap-local redesign

### Candidate C: direct leaf-slot identity plus parent-PT liveness

This is the smallest redesign that currently looks complete enough to matter.

It combines:

1. cap-local direct leaf-slot identity for frame mappings
2. explicit parent page-table liveness/ownership so that the stored slot
   identity cannot silently become a pointer into reused memory

Why this looks like the first complete design:

1. `unmapPage()` no longer needs to rediscover the leaf slot through a live
   walk
2. but the design still acknowledges that the parent PT object itself must stay
   meaningful until all child frame mappings that point into it are retired
3. without that second piece, a stored slot pointer is just another stale
   object-identity hazard

### What looks implausible now

Two seemingly attractive variants look weak on current source evidence:

1. compress exact `vaddr` enough to keep both exact `vaddr` and full direct
   slot identity in the existing 48-bit cap field
   - on the active 40-bit user-VA configuration, there is not enough room for
     both exact `vaddr` and parent-PT identity
2. “just store the leaf slot pointer in the cap and stop there”
   - that does not solve parent PT lifetime/reuse

## Current Redesign Ranking

If option 2 becomes primary, the redesign candidates now rank like this:

1. most plausible complete redesign:
   - direct leaf-slot identity plus explicit parent-PT liveness
2. smaller but incomplete redesign:
   - cap-local direct leaf-slot identity alone
3. semantically cleaner but larger redesign:
   - reverse-map side metadata

## Practical Readout

The smallest redesign worth taking seriously is no longer “more bits in the
cap” in the abstract. It is:

1. stop storing exact mapped `vaddr` as the primary identity for mapped frame
   caps
2. store direct leaf-slot identity instead
3. add enough page-table liveness structure that the stored slot identity
   remains trustworthy until the frame mapping is retired

Anything smaller currently looks likely to recreate the same trust problem one
level lower.

## Minimal Parent-PT Liveness Design

The remaining question is what “explicit parent-PT liveness” minimally means in
seL4 source terms.

The current page-table-cap state is too weak for this purpose.

Today a page-table cap stores:

1. parent mapping ASID
2. page-table object base pointer
3. one `capPTIsMapped` bit
4. mapped virtual address base

See
[structures.bf](/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/64/mode/object/structures.bf#L36).

That is enough to say:

1. this PT object is currently linked into a parent slot

But it is not enough to say:

1. some mapped frame caps still rely on this PT object as the trusted parent of
   their stored leaf-slot identity
2. therefore this PT object cannot yet be retired, reused, or have its parent
   slot meaning changed

### The minimum new invariant

The smallest plausible liveness invariant is:

1. each mapped leaf-frame identity carries an associated parent PT object
2. that parent PT object remains live and non-reusable until all leaf mappings
   that name slots within it have been retired

In source terms, that implies a new distinction:

1. PT is mapped in the translation tree
2. PT is still referenced by live mapped-frame metadata

Current seL4 only has the first.

### Smallest plausible mechanism

The smallest mechanism that currently looks coherent is a per-PT live-leaf
reference count or pin.

Conceptually:

1. when a frame mapping is installed into a PT slot:
   - increment that PT's live-leaf count
2. when a frame mapping is retired:
   - decrement that PT's live-leaf count
3. page-table unmap/finalise/reuse may proceed only when:
   - `capPTIsMapped == 0` at the translation-tree level, and
   - live-leaf count is `0` at the metadata-dependence level

This is the minimum design that prevents the obvious stale-identity failure:

1. a frame cap stores direct leaf-slot identity into PT `P`
2. PT `P` is unmapped and later reused
3. the stored slot identity now points into unrelated memory

Without explicit PT liveness, direct slot identity alone is unsafe against that
sequence.

### Why `capPTIsMapped` is not enough

Current PT teardown paths show the gap directly:

1. explicit PT unmap goes through
   [performPageTableInvocationUnmap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1217)
   and then clears/sanitises the PT memory
2. final-cap PT deletion goes through
   [Arch_finaliseCap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c#L178)
   and also sanitises the PT memory
3. neither path currently has any notion that unrelated mapped frame caps may
   still depend on this PT object as their trusted parent identity source

So a single `capPTIsMapped` bit is only a tree-link property, not a
dependency-liveness property.

### Smallest place to hang the liveness state

There are two plausible homes for the new state:

1. in the PT object memory itself
2. in side metadata keyed by PT base pointer

The smallest design that looks compatible with current object model is side
metadata keyed by PT base pointer.

Why:

1. PT object memory is translation memory and already part of the coherency
   problem
2. embedding liveness counters inside the PT page itself would mix management
   state into memory that is also walker-visible translation state
3. side metadata avoids making the translation page format even more fragile

### Minimal lifecycle rules

If this redesign were pursued, the minimum rules would be:

1. map frame:
   - derive direct leaf-slot identity
   - record it in the frame cap
   - increment parent PT live-leaf count
2. unmap frame / finalise frame cap:
   - retire the slot by direct identity
   - decrement parent PT live-leaf count only after retirement is complete
3. unmap PT / finalise PT cap:
   - allowed to remove the parent-tree link only when architecturally valid
   - not allowed to free/reuse the PT object while live-leaf count is nonzero
4. untyped reset/retype:
   - PT object reuse only after both tree-link state and live-leaf liveness
     state say the object is dead

### Net result

The smallest parent-PT liveness design that currently looks complete enough is:

1. frame caps store direct leaf-slot identity
2. side metadata keyed by PT base pointer tracks a live-leaf pin/count
3. PT reuse is blocked until that count reaches zero

This is larger than a cap-only redesign, but still materially smaller than a
full reverse-map database for every mapping.

## Implementable Design Sketch

This section turns the redesign direction into a concrete kernel-shaping plan.

### New state

The minimum new state is:

1. frame-cap mapping identity:
   - replace exact stored mapped `vaddr` with direct leaf-slot identity
   - minimum content:
     - parent PT base page number
     - leaf slot index
2. parent-PT liveness metadata:
   - side metadata keyed by PT base pointer
   - minimum field:
     - live-leaf reference count

Optional later additions, not required for the first cut:

1. generation counter on PT metadata
2. explicit “retirement pending” flag for debugging/invariants

### Exact kernel touch points

The first implementation would have to touch at least these areas.

#### 1. Cap layout and generated accessors

Files:

1. [structures.bf](/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/64/mode/object/structures.bf)
2. generated accessors in
   [structures_gen.h](/home/hlyytine/tii-sel4/orinagx_sel4test/kernel/generated/arch/object/structures_gen.h)

Work:

1. redefine the `capFMappedAddress` payload into direct leaf-slot identity
2. regenerate and update all cap accessor call sites

#### 2. Arch state data for PT liveness

Files:

1. [statedata.h](/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/64/mode/model/statedata.h)
2. [statedata.c](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/model/statedata.c)

Work:

1. add side metadata keyed by PT base pointer
2. provide lookup/init/reset helpers

This is the natural home because existing Arm arch-global translation state
already lives there.

#### 3. Frame map / unmap paths

Files:

1. [vspace.c](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c)
2. [objecttype.c](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c)

Work:

1. on frame map in
   [decodeARMFrameInvocation()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1539):
   - derive direct leaf-slot identity from `lu_ret.ptSlot`
   - store it in the frame cap
   - increment parent PT live-leaf count
2. on page unmap in
   [performPageInvocationUnmap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1245)
   and final-cap frame teardown in
   [Arch_finaliseCap()](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c#L190):
   - retire by direct slot identity, not by live `lookupPTSlot()` rediscovery
   - decrement parent PT live-leaf count only after retirement completion

#### 4. Page-table unmap/finalise/reuse paths

Files:

1. [vspace.c](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c)
2. [objecttype.c](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c)
3. [untyped.c](/home/hlyytine/tii-sel4/kernel/src/object/untyped.c)

Work:

1. block PT unmap/finalise if live-leaf count is nonzero, or split it into:
   - unlink from tree
   - delay free/reuse until count reaches zero
2. ensure final PT sanitisation does not race with outstanding direct leaf-slot
   identities
3. ensure untyped reset/retype cannot reuse a PT object while the liveness
   metadata still says it is depended on

#### 5. Page flush and ancillary users of mapped identity

Files:

1. [vspace.c](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c)
2. [capdl.c](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/machine/capdl.c)

Work:

1. adapt page flush operations that currently use exact stored `vaddr`
2. adapt debug/capDL reporting that expects exact mapped address in the cap

These are not the hardest correctness points, but they are mandatory fallout
from replacing `capFMappedAddress`.

### Minimal invariants to enforce

The first complete implementation must make these invariants explicit:

1. every mapped frame cap with nonzero mapped-ASID has a valid direct leaf-slot
   identity
2. the parent PT named by that identity has positive live-leaf liveness
3. a PT object with positive live-leaf liveness cannot be reused
4. decrement of PT live-leaf liveness happens only after leaf retirement is
   complete
5. frame-cap unmap/finalise does not need to rediscover the leaf by live tree
   walk in the common case

### Likely commit split

The smallest reviewable series would probably be:

1. `D1`: add PT liveness side metadata and helper API
   - no cap format changes yet
   - establish init/reset/lookups and debug assertions
2. `D2`: change frame-cap mapping identity format
   - replace exact stored `vaddr` with direct leaf-slot identity
   - regenerate accessors and adapt callers
3. `D3`: wire frame map/unmap/finalise to PT liveness
   - increment on map
   - retire by direct slot identity
   - decrement after retirement completion
4. `D4`: gate PT finalise/untyped reuse on PT liveness
   - prevent premature PT reuse
5. `D5`: clean up secondary fallout
   - page flush, capDL/debug paths, comments/docs

### Recommended sequencing

The safest order is:

1. land metadata first
2. then land cap identity changes
3. then switch the dominant `unmapPage()` path over
4. only then gate PT teardown/reuse hard

Reason:

1. the dominant risk is still the frame-cap teardown reader
2. but that reader cannot be switched safely until both identity and PT
   liveness exist

### `D1` status

The first redesign step is now implemented as metadata scaffolding only:

1. a PT-liveness side table keyed by PT base pointer now exists in Arm64 arch
   state
2. helper APIs now exist for:
   - ensure tracked
   - forget
   - increment
   - decrement
   - get count
3. current non-invasive hook points:
   - PT map ensures tracking exists
   - PT unmap/finalise assert zero live-leaf count before sanitise/forget
4. this does not yet change frame-cap identity or `unmapPage()` behavior
5. clean Orin `sel4test` build passed after the change

### `D2` status

The second redesign step is now implemented as the identity transition:

1. runtime-mapped frame caps now store tagged direct leaf-slot identity instead
   of exact mapped `vaddr`
2. PT-liveness metadata now also records each tracked parent table's mapped
   `asid` and `vaddr` base
3. transitional slow paths that still need exact `vaddr` reconstruct it from:
   - stored parent-slot identity in the frame cap
   - mapped-base metadata for the parent PT/root
4. remap validation now compares direct slot identity instead of exact-address
   equality
5. boot-created frame caps remain legacy/raw-`vaddr` encoded and are handled by
   the same helper path
6. clean Orin `sel4test` build passed after the change

### `D3` status

The third redesign step is now implemented, but not yet correct:

1. first publication of a leaf into an invalid slot now increments the tracked
   parent PT's live-leaf count
2. runtime frame unmap and frame-cap finalisation now first try to retire by
   stored direct slot identity instead of rediscovering the leaf through a live
   `lookupPTSlot()` walk
3. legacy / untagged frame mappings still fall back to the older
   `unmapPage()` path
4. clean Orin `sel4test` build passed after the change
5. extended runtime validation reached seL4, reproduced the older low-address
   `SCC` + `ACI` RAS family, and then failed on a new liveness assertion in
   `Arch_finaliseCap()`
6. current interpretation:
   - the direct-slot retirement path is active enough to expose real PT
     liveness bookkeeping
   - but the current `D3` accounting is incomplete, so PT finalisation can
     still see a nonzero live-leaf count

### `D3` follow-up status

The immediate PT-liveness accounting follow-up was implemented in
`d29ae0338`, but it uncovered a different class of regression:

1. the old PT-finalisation assertion is removed
2. the rebuilt image now faults before seL4 reaches `Bootstrapping kernel`
3. the early exception is source-backed as a real fault at the elfloader's
   `flush_dcache_range()` `dc civac, x0` instruction, not an arbitrary UEFI
   message
4. because `d29ae0338` only changed kernel files, the best current reading is
   that this is an indirect payload/layout-sensitive handoff regression rather
   than the new PT-liveness logic executing directly

## Remaining Proof Gaps

The audit is strong enough to rank explanations, but not to claim final proof
yet.

Main remaining gaps:

1. whether fresh translation-table publication with `PoU` is merely
   conservative debt or a direct contract violation for Orin-class systems
2. the exact Arm-source-backed answer for the remaining rights-only /
   executability-only valid-to-valid remap subset

## Practical Readout

If you need one short conclusion:

The most plausible current source-only explanation is that Orin seL4 RAS errors
come from translation metadata being retired while live walk/decode activity is
still colliding with EL2 translation-maintenance sequencing. The creation/final-
delete fixes and the later live-slot refactoring/remap split both changed the
runtime shape, but neither removed the failure. Reset/reissue and staged
teardown still matter, but the current highest-priority remaining target is the
surviving walk/decode family itself; `K6` suggests the remaining issue will not
be solved by another small EL2-TLBI helper adjustment alone.
