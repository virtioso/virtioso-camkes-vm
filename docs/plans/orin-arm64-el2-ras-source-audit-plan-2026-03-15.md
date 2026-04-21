# Orin AArch64 EL2 RAS Source Audit Plan

## Purpose

Establish a source-only audit plan for why seL4 on NVIDIA Orin AGX keeps
generating RAS errors in AArch64 hypervisor mode.

This plan is the canonical tracker for the audit work. It is intended to force
reasoning from kernel source, ARM architecture rules, and primary vendor
sources before any implementation or hardware activity is considered.

## Constraints

This workstream is source-first only.

Allowed evidence:

1. seL4 kernel source and generated headers
2. existing local investigation notes and raw postprocessed traces
3. ARM architecture documentation and primary upstream/kernel/vendor sources

Explicitly out of scope for this plan:

1. new hardware experiments
2. new instrumentation intended only to "see what happens"
3. validating hypotheses by trial on the board before the source audit is
   decision-complete

## Working Position

The current evidence does not justify reducing the Orin RAS problem to one
cause.

The audit must evaluate at least two explanation classes in parallel:

1. architectural translation-maintenance violations
   - `VTTBR_EL2` / `TTBR0_EL2` switching
   - TLBI ordering and completion
   - speculative page-table walks and transiently unsafe entries
2. ordinary kernel metadata lifetime/publication failures
   - stale or prematurely reused page-table memory
   - stale ASID/HW-VMID state
   - stale MDB/capability graph state
   - detector sites such as `pte_ptr_get_pte_type()`, `findMapForASID()`, and
     `isFinalCapability()` consuming already-bad state

The audit should end with one ranked explanation, not a list of equal-weight
possibilities.

## Why This Audit Is Needed

Recent postprocessing of the latest autopilot run showed repeated PCs in:

1. `unmapPage()`
2. `pte_ptr_get_pte_type()`
3. `findMapForASID()`
4. `isFinalCapability()`

That spread is too broad to treat as "just PTW". It points to a wider audit
surface covering:

1. translation-regime switching
2. live PTE mutation and TLBI
3. page-table object creation, clearing, unmap, and reuse
4. non-PTE metadata readers in ASID and MDB logic

## Audit Questions

The audit must answer these questions explicitly.

### Q1. Translation-regime contract

When AArch64/EL2 code changes `VTTBR_EL2`, `TTBR0_EL2`, or related regime
state, does the source satisfy the architectural requirements for completion of
older `EL0/EL1` walks and safe observation of the new regime?

### Q2. Live PTE mutation contract

When seL4 edits active page tables, does the code satisfy the required ordering
for:

1. break-before-make or invalidation
2. cache maintenance visibility
3. TLBI scope
4. PTW completion
5. memory reuse after unmap

### Q3. Object lifecycle contract

When a frame is retyped into a page table or a page table is cleared and later
reused, does the source ensure that no stale prior-owner contents remain
visible to the walker or to later kernel readers?

### Q4. Non-PT metadata contract

Can ASID tables, HW VMID tables, or MDB state become stale or transiently
inconsistent in ways that later drive valid-looking but wrong page-table or cap
reads?

### Q5. Commit usefulness

Is commit `eba5a3b6e6b804a77c6cfc8d2a6535707c953c9a` materially helpful, only
partially helpful, or misleading in some way?

The audit must answer that both at the code level and at the explanation level.

## Initial Read On Commit `eba5a3b6e6b804a77c6cfc8d2a6535707c953c9a`

Current provisional reading from source review:

1. The code direction is likely helpful on Orin.
   - Switching page-table maintenance call sites from `PoU`-style cleaning to
     `dc civac`/`PoC` is a conservative visibility choice for complex Tegra
     cache hierarchies and for page-table memory being reused from prior data.
2. The commit message is too strong as written.
   - It states that the hardware walker reads from `PoC`, which overstates the
     architectural claim.
   - The safer wording is that `PoC` is used as a conservative implementation
     choice on Orin and similar systems, not that architecture universally
     defines the walker as a `PoC` consumer.
3. The commit is incomplete as a root-cause fix.
   - It addresses one visibility dimension.
   - It does not by itself prove correct barrier composition around
     `VTTBR_EL2` changes, TLBI completion, or page-table memory reuse.
4. The commit should therefore be audited as:
   - probably useful code
   - likely overstated rationale
   - insufficient if presented as the full explanation

This provisional judgement must be confirmed or overturned by the audit.

## Audit Workstreams

### W1. Translation-regime switch audit

Audit all AArch64/EL2 paths that can change active translation regime state.

Must-cover functions:

1. `setCurrentUserVSpaceRoot()`
2. `setCurrentKernelVSpaceRoot()`
3. `armv_contextSwitch()`
4. `invalidateLocalTLB_VMID()`
5. `invalidateLocalTLB_IPA_VMID()`
6. `invalidateTranslationASID()`
7. `invalidateTranslationSingle()`
8. `vcpu_switch()` and any helper that affects active stage-2 state

Required output per function:

1. source path
2. architectural contract being relied on
3. preconditions assumed by the caller
4. barrier and TLBI sequence actually present
5. whether that sequence is:
   - sound
   - sound only under an unstated assumption
   - suspicious
   - irrelevant to the current RAS family

### W2. Live PTE mutation and reuse audit

Audit all AArch64 page-table creation, mutation, unmap, delete, and clear
paths.

Must-cover sites:

1. page-table object creation in `kernel/src/arch/arm/64/object/objecttype.c`
2. `unmapPageTable()`
3. `unmapPage()`
4. `deleteASID()`
5. `deleteASIDPool()`
6. `performPageTableInvocationMap()`
7. `performPageInvocationMap()`
8. `clearMemory_PT()`
9. all uses of `pte_pte_invalid_new()`

Required output:

1. exact source order
2. intended architectural order
3. whether invalid entries are speculation-safe
4. whether cache maintenance point is justified
5. whether TLBI completes before any reuse or further publication
6. whether the code relies on helper semantics that are undocumented

### W3. Non-PTE metadata audit

Audit metadata readers and writers that can participate in the same failure
windows as page-table changes.

Must-cover sites:

1. `findMapForASID()`
2. `getHWASID()`, `storeHWASID()`, `invalidateASID()`, `invalidateASIDEntry()`
3. ASID table readers in `kernel/src/fastpath/fastpath.c`
4. `isFinalCapability()`
5. MDB mutations around delete/revoke/finalisation

Required output:

1. writer paths
2. reader paths
3. synchronization/lifetime assumptions
4. whether stale-but-well-typed metadata could explain the observed PCs better
   than pure PTW

### W4. Commit `eba5a3b6...` review track

Review the commit as both code and explanation.

Required questions:

1. Which bugs does the code plausibly reduce?
2. Which bug families can still happen even if the commit is correct?
3. Which claims in the commit message or comments should be rewritten?
4. Should the fix stay as-is, be narrowed, or be split into:
   - a conservative implementation change
   - a separately worded rationale change

Required output:

1. keep / rewrite / split recommendation
2. exact reasons
3. any proposed wording changes

## Evidence Base

Primary local sources to anchor the audit:

1. `kernel/include/arch/arm/arch/64/mode/machine.h`
2. `kernel/include/arch/arm/arch/machine/tlb.h`
3. `kernel/include/arch/arm/armv/armv8-a/64/armv/tlb.h`
4. `kernel/include/arch/arm/armv/armv8-a/64/armv/context_switch.h`
5. `kernel/src/arch/arm/64/kernel/vspace.c`
6. `kernel/src/arch/arm/64/object/objecttype.c`
7. `kernel/src/object/cnode.c`
8. `kernel/src/fastpath/fastpath.c`
9. `docs/platforms/orin-agx/reference/arm-speculative-ptw-research.md`
10. `docs/platforms/orin-agx/ras-errors.md`
11. `docs/platforms/orin-agx/investigations/page-table-barrier-audit.md`
12. `docs/plans/orin-kernel-camkes-unified-memory-contract-audit-plan-2026-03-12.md`

Primary external source classes:

1. ARM ARM rules for out-of-context translation regimes and TLBI/barrier
   requirements
2. Linux/KVM arm64 speculative PTW and translation-regime fixes
3. Xen ARM `AT_SPECULATE` handling and empty-root-table pattern
4. NVIDIA/Tegra TF-A RAS handling and node/status definitions where relevant

## Acceptance Criteria

The audit is complete only when all of the following are true:

1. Every listed workstream has a written verdict.
2. No helper remains with an implicit barrier/TLBI contract.
3. The audit explains the hot PCs as origin sites, detector sites, or both.
4. The audit gives one ranked root-cause model.
5. The audit states whether `eba5a3b6e6b804a77c6cfc8d2a6535707c953c9a` should:
   - stay as-is
   - stay with rewritten rationale/comments
   - be partially reverted/reworked

## Progress Tracker

Status values:

1. `planned`
2. `in_progress`
3. `done`
4. `blocked`

| Step ID | Goal | Status | Reasoning / sources | Repos touched | Files expected or touched | Hypothesis / question | Acceptance check | Commit SHA(s) | Evidence |
|---|---|---|---|---|---|---|---|---|---|
| A1 | Establish the source-only audit scope and workstreams | done | kernel source; existing Orin RAS/PTW notes; latest postprocessed hot PCs | `projects/virtioso-camkes-vm` | this plan | The observed RAS family spans more than one mechanism and needs a contract audit rather than isolated fixes | Workstreams W1-W4 are defined and tied to concrete source roots | pending | this document |
| A2 | Review commit `eba5a3b6...` at source level and decide whether it is directionally useful | done | `git show eba5a3b6...`; current `kernel/include/arch/arm/arch/machine.h`; current `kernel/src/arch/arm/64/kernel/vspace.c`; Linux AArch64 boot/coherency guidance on cleaning to PoC in presence of system caches; existing KVM speculative-PTW notes | `kernel` | no edits yet | The commit is probably useful but overstates its architectural rationale and is incomplete as a full fix | Produce keep/rewrite/split recommendation with exact reasons | `eba5a3b6e6b804a77c6cfc8d2a6535707c953c9a` | local commit diff, current source, Linux boot/coherency docs |
| A3 | Audit translation-regime switching and TLBI helper contracts | in_progress | ARM ARM + KVM/Xen + seL4 EL2 helper code; SMP remote-TLBI helper paths | `kernel` | audit notes or follow-up doc | A missing or weak ordering contract around `VTTBR_EL2`/TLBI remains a plausible contributor | Every listed EL2 helper receives a written verdict | pending | source files listed in W1 |
| A4 | Audit live PTE mutation, invalid entries, cache maintenance, and reuse | in_progress | seL4 vspace/object code + prior Orin notes; current creation/unmap/map call sites; ARM guidance that first-time invalid-to-valid publication does not require TLBI | `kernel` | audit notes or follow-up doc | The strongest remaining runtime mismatch is not first map, but runtime VSpace-root publication/reuse with `PoU`, plus legacy valid-to-valid leaf replacement ordering | Mutation/reuse sequences are written down and ranked by suspicion | pending | source files listed in W2; current plan notes |
| A5 | Audit ASID/HW-VMID/MDB metadata lifetime and stale-reader hazards | in_progress | seL4 cnode, fastpath, ASID code; current MDB mutation and fastpath validation paths | `kernel` | audit notes or follow-up doc | The hot PCs in `findMapForASID()` and `isFinalCapability()` are detector sites for stale metadata, not just unrelated victims | Writer/reader pairs and assumptions are documented, and post-delete logical reachability is clarified | pending | source files listed in W3; current plan notes |
| A6 | Produce final ranked explanation and commit recommendation | planned | outputs of A2-A5 | `projects/virtioso-camkes-vm` and possibly `kernel` if rationale rewrite is proposed later | final audit write-up | One explanation class should dominate once helper contracts and metadata lifetime are audited together | Final audit names the most plausible explanation and the disposition of `eba5a3b6...` | pending | final audit result |

## Current Progress Summary

Completed:

1. Defined the audit scope and the mandatory workstreams.
2. Confirmed that commit `eba5a3b6e6b804a77c6cfc8d2a6535707c953c9a` lives in the
   `kernel` repo.
3. Performed an initial source read of the commit and the current AArch64/EL2
   helper paths it depends on.

Current active item:

1. Steps `A3`, `A4`, and `A5` are in progress.

Current tracked sub-results:

1. `A4` has established:
   - invalid-to-valid first publication is a weaker suspect because faulting
     translations are not expected to be cached
   - runtime `VSpace` creation/reuse still publishes fresh roots with `PoU`
   - runtime ASID assignment does not add a later publication fixup for those roots
   - `performPageInvocationMap()` remains the strongest single-function remap-ordering concern
   - upstream Linux arm64/KVM separates first publication from live remap:
     new table pages are made visible to the walker explicitly, while broader
     live mapping changes use BBM-style retirement unless the change is in a
     narrowly defined safe-update category
   - on current AArch64+hypervisor seL4, the remap path's live-change surface
     is now classified more precisely:
     - `armPageCacheable` changes stage-2 memory type/cacheability and
       shareability, which sits in Arm's explicitly BBM-sensitive class
     - `armExecuteNever` changes executability
     - rights changes alter stage-2 access permissions
     - `armParityEnabled` exists in the ABI but is not used by the AArch64
       `makeUserPagePTE()` path
     - the executability subset may be partially governed by a separate
       seL4 instruction-cache maintenance contract via
       `ARMPageUnify_Instruction` / `ARMVSpaceUnify_Instruction`, rather than
       by `performPageInvocationMap()` alone
     - current API/manual shape supports that reading: seL4 exposes
       instruction-cache unification as an explicit separate operation, and
       the AArch64 implementation performs the D-to-I PoU clean/invalidate
       sequence there rather than in the map path
   - source-usage scan suggests cacheability-changing mappings are real but not
     obviously hot in the failing workloads:
     - most common helper layers use `seL4_ARM_Default_VMAttributes`
     - uncached mappings appear in specialised flows such as DMA / VM support
     - those specialised flows generally look like first-map/setup paths, not
       repeated same-frame same-VA remap
   - earlier sel4test evidence is strong enough to steer weighting:
     - only a small minority of sel4test cases were observed to trigger RAS
       at all
     - the dominant triggers clustered in `CANCEL_BADGED_SENDS_0002` and
       `FPU0001`
     - those tests stress thread wake/resume cascades, revoke/cleanup, and
       repeated object creation/mapping far more than they stress unusual
       cacheability remap patterns
     - this strengthens lifecycle/publication/reuse and delayed-detector
       interpretations relative to niche remap-only theories
   - current trigger-test source review narrows the hottest shared-state churn:
     - `FPU0001` and `CANCEL_BADGED_SENDS_0002` create and destroy helper
       threads, not fresh helper processes, in their hot paths
     - helper-thread setup uses `sel4utils_configure_thread_config()` against
       the existing `env->vspace`
     - each helper thread allocates:
       - one TCB object
       - one IPC-buffer page unless explicitly suppressed
       - `stack_size` 4 KiB stack pages plus a guard reservation
     - helper-thread teardown uses `sel4utils_clean_up_thread()`, which frees
       TCBs plus IPC-buffer and stack pages in that same shared vspace
     - stack allocation/free runs through `vspace_new_sized_stack()` /
       `vspace_free_sized_stack()` and the underlying shared-vspace
       map/unmap/free path
     - this makes shared-vspace page/frame/page-table lifecycle in a long-lived
       root a better fit to the trigger tests than repeated fresh top-level
       VSpace-root publication
   - the two dominant trigger tests now separate into different lifecycle
     shapes:
     - `FPU0001` does repeated create/start/wait/`cleanup_helper()` cycles for
       4 helper threads until enough preemptions occur
     - `CANCEL_BADGED_SENDS_0002` creates 32 helper threads plus a reply
       thread, two endpoint-derived caps per helper, and one sync endpoint per
       helper, then repeatedly executes `cnode_revoke()` and
       `cnode_cancelBadgedSends()` sweeps without per-helper cleanup in the
       test body
   - this matters for interpretation:
     - `FPU0001` is the clean workload match for repeated helper-thread
       frame/vspace churn
     - `CANCEL_BADGED_SENDS_0002` is a cleaner workload match for
       endpoint/MDB/cap-delete pressure first, with vspace teardown pressure
       deferred until later
   - the allocator backend for those trigger tests is now confirmed to be
     freeable split-utspace allocman, not a leak-only backend:
     - sel4test test instances bootstrap with `bootstrap_use_current_1level()`
       and then add the handed-over untypeds directly into allocman
     - the driver side bootstraps with `bootstrap_use_current_simple()`
     - libsel4allocman hardwires these bootstrap paths to `utspace_split`
     - `utspace_split` returns freed allocations back into the split tree and
       coalesces buddies on free
   - the shared-vspace helper-thread path returns freed frame cookies
     immediately into that allocator regime:
     - `new_pages_at_vaddr()` allocates frames with
       `vka_alloc_frame_maybe_device()`
     - `sel4utils_unmap_pages()` unmaps the page, deletes the cap, frees the
       cslot, and then calls `vka_utspace_free()` on the saved frame cookie
   - missing paging structures also come from the same broad VKA/allocman
     regime:
     - `sel4utils_map_page()` allocates missing paging objects with
       `vka_alloc_object()`
     - sel4test's `vka` is allocman-backed
   - this does not yet prove one specific frame must later become one specific
     page-table object, but it is strong source evidence that the dominant
     trigger tests exercise one shared split-backed untyped pool in which page
     frames and paging structures are both allocated, freed, and recycled
   - the current helper-thread source path also narrows what probably does
     and does not churn in the hottest loops:
     - stack/IPC teardown clears sel4utils bookkeeping entries and frees the
       frame cookies back to allocman
     - `clear_entries_mid()` only empties the sel4utils metadata tree; it does
       not retire those bookkeeping levels
     - `find_range()` resets `last_allocated` back toward the freed range, so
       the next helper-thread stack allocation is likely to reuse the same
       virtual-address window
     - therefore the dominant hot loops are a much stronger fit for repeated
       4 KiB frame churn than for repeated kernel page-table creation on every
       iteration
2. `A5` has established:
   - `findMapForASID()` and fastpath ASID checks are integrity readers, not
     obvious origin sites
   - `isFinalCapability()` is likewise a detector-style reader over MDB state
   - after `deleteASID()`, stale roots are no longer logically reachable via
     the ASID table, strengthening the later-reuse interpretation
   - the userspace process-destruction path has its own split teardown shape:
     - `sel4utils_destroy_process()` runs `sel4utils_clean_up_thread()` first
     - then `vspace_tear_down()`
     - then only afterwards frees objects accumulated through the vspace
       `allocated_object` callback via `clear_objects()`
   - this matters because libsel4utils records paging-structure objects
     through that callback when missing paging levels are allocated during map
     operations
   - on the active Orin AArch64 hypervisor build
     (`CONFIG_ARM_HYPERVISOR_SUPPORT`, `CONFIG_ARM_PA_SIZE_BITS_40`), those
     callback-fed objects are specifically Arm paging structures returned from
     `seL4_FailedLookup` handling:
     - `seL4_ARM_PageTableObject`
     - `seL4_ARM_PageDirectoryObject`
     - not ordinary frame objects
   - the later `clear_objects()` pass does not invent a separate disposal
     mechanism for them:
     - `vka_free_object()` issues `seL4_CNode_Delete` on the recorded object
       cap
     - then frees the cslot
     - then returns the untyped cookie via `vka_utspace_free()`
   - so when those recorded paging-object caps are final caps, `clear_objects()`
     simply becomes another delayed entry into the ordinary kernel
     `cteDelete()` / `finaliseCap()` machinery
   - `sel4utils_tear_down()` recursively frees mapped pages and unmaps the
     sel4utils bookkeeping tree from the bootstrap vspace, but the separately
     tracked kernel paging objects are only freed in the later `clear_objects()`
     pass
   - source order makes the phase split explicit:
     - `sel4utils_destroy_process()` calls `vspace_tear_down()` first
     - inside that, `free_pages_at_level()` reaches `free_page()`
     - `free_page()` ultimately routes through `sel4utils_unmap_pages()`,
       which issues page unmap, cslot delete, and frame-cookie utspace free
     - only after `vspace_tear_down()` returns does `clear_objects()` walk the
       recorded PT/PD object list
   - there is no matching PT/PD unmap step inside `vspace_tear_down()` itself:
     - it does not call `seL4_ARM_PageTable_Unmap`
     - it does not call `seL4_ARM_PageDirectory_Unmap`
     - it walks leaf mappings plus bootstrap-vspace bookkeeping mappings
   - so for the recorded paging objects, `clear_objects()` is the point where
     cap deletion of still-recorded PT/PD objects actually begins
   - that gives the teardown path another concrete "retire one layer now, free
     another layer later" asymmetry, which fits the delayed-detector reading
     for `unmapPage()` and `pte_ptr_get_pte_type()`

Current default recommendation for `eba5a3b6...` unless later evidence changes
it:

1. keep the conservative `PoC` code change for now
2. rewrite the explanatory comments/commit rationale to avoid claiming that the
   architecture simply defines the PTW as a `PoC` consumer
3. do not treat the commit as a complete fix until W1-W3 are complete
4. if implementation changes are made later, prefer to present them as:
   - runtime mutation / retirement visibility cleanup
   - creation-time publication cleanup
   - separate transition-ordering review for valid-to-valid remap

Additional note from current source-and-web review:

1. Linux AArch64 documentation explicitly requires cleaning the loaded kernel
   image to `PoC` in the presence of system caches and coherent masters, which
   supports `PoC` as a conservative implementation choice on complex SoCs.
2. That does not settle the separate seL4 question of whether the remaining
   EL2 translation-regime and TLBI helper barriers are sufficient.

Additional note from current translation-helper review:

1. The present AArch64/EL2 helper paths do not show an obvious "missing barrier
   entirely" defect in `setCurrentUserVSpaceRoot()`, `invalidateLocalTLB_VMID()`,
   or the synchronous remote-TLBI wrapper path.
2. The main weakness so far is that the safety argument is implicit across
   helper layers rather than stated as one contract.
3. Unless later evidence overturns this, W2 and W3 currently look more
   suspicious than W1 as the dominant explanation for the remaining RAS family.

Additional note from current page-table lifecycle review:

1. Creation-time page-table initialization is still inconsistent with the later
   conservative `PoC` policy.
2. In `kernel/src/arch/arm/64/object/objecttype.c`, new `VSpace` and
   `PageTable` objects are filled with `pte_pte_invalid_new()` and then cleaned
   only with `cleanCacheRange_PoU()`.
3. In boot-time address-space construction, parent links created by
   `map_it_pud_cap()`, `map_it_pd_cap()`, `map_it_pt_cap()`, and
   `map_it_frame_cap()` are written without adjacent cache-maintenance calls.
4. This makes `eba5a3b6...` look even more like a partial visibility cleanup
   than a complete model of the page-table contract.
5. The current source tree defines `pte_pte_invalid_new()` as a safe invalid
   entry using `armKSGlobalUserVSpace`, so the remaining issue is no longer
   "zero invalids" at these sites but whether safe invalid entries are
   published, observed, and retired with the correct cache/TLBI/lifetime
   contract.

Additional note from current metadata-reader review:

1. `findMapForASID()` and the fastpath ASID checks are plain integrity readers
   over ASID metadata, but they do not currently look like free-standing SMP
   race sites.
2. `isFinalCapability()` is likewise a straightforward reader over neighboring
   MDB links and caps; the source read so far does not point to it as an origin
   of corruption.
3. The current stronger reading is that these hot PCs are more likely detector
   sites consuming earlier stale logical state than primary root-cause sites.

Additional note from current reuse-chain review:

1. The strongest remaining suspicion is the full lifecycle chain, not a single
   helper in isolation.
2. For page-table pages, the relevant sequence spans:
   - parent-entry unmap
   - cache maintenance on that entry
   - TLBI
   - later `clearMemory_PT()` of the child table memory
   - cap/MDB detachment and `postCapDeletion()`
   - eventual untyped reset and `Arch_createObject()` reissue
3. If any link in that chain assumes more global completion or visibility than
   the previous link actually guaranteed, stale state can survive even when the
   local helper bodies each look individually reasonable.
4. There is also an object-type asymmetry in the current source:
   - explicit page-table unmap invocation later calls `clearMemory_PT()` on the
     child table memory
   - VSpace finalisation only removes the ASID mapping and does not explicitly
     republish the root table memory into a safe state at that point
5. That makes VSpace-object reuse a particularly plausible place where stale
   translation metadata can survive until a later generic reset/retype stage.
6. The current source also makes the post-delete logical picture clearer:
   - `deleteASID()` removes the ASID-table entry and resets the current active
     hardware root via `setVMRoot()`
   - later readers such as `findMapForASID()` and the fastpath no longer reach
     the old root through the logical ASID mapping
7. Therefore, if stale VSpace-root contents matter after deletion, the stronger
   explanation is later reuse/publication of old memory contents, not continued
   logical reachability through the ASID table.
8. The runtime reissue path is exact and confirmed in source:
   - untyped retype calls `createNewObjects()`
   - that calls `createObject()`
   - which calls `Arch_createObject()` for `seL4_ARM_VSpaceObject`
   - and `Arch_createObject()` currently initializes the new root then flushes
     it only with `cleanCacheRange_PoU()`
9. So the VSpace-root reuse/publication concern is not hypothetical; it is the
   precise current runtime creation path for fresh roots.
10. The runtime reachability step also does not add an obvious later fixup:
    assigning a fresh `VSpace` into an ASID pool makes it logically reachable,
    but the current `ARMASIDPoolAssign` path does not perform an additional
    cache-maintenance publication step over the new root at that point.

Current source-order chains recorded for W2:

1. Page-table map publication:
   - `performPageTableInvocationMap()`
   - store parent PTE
   - `cleanInvalByVA()` on the parent slot
   - return
   - no explicit TLBI on the new-table publication path
2. Page map / remap publication:
   - `performPageInvocationMap()`
   - compute `tlbflush_required`
   - store new leaf PTE
   - `cleanInvalByVA()` on the leaf slot
   - if replacing an existing valid entry, `invalidateTLBByASIDVA()`
   - return
3. Page-table unmap and later reuse:
   - `unmapPageTable()`
   - store safe invalid parent PTE
   - `cleanInvalByVA()` on parent slot
   - `invalidateTLBByASID()`
   - later `performPageTableInvocationUnmap()`
   - `clearMemory_PT()` over child table memory
   - cap mapped bit cleared in the slot
   - later cap finalisation / slot emptying / untyped reset / `Arch_createObject()`
4. Leaf page unmap:
   - `unmapPage()`
   - store safe invalid leaf PTE
   - `cleanInvalByVA()` on leaf slot
   - `invalidateTLBByASIDVA()`
   - later frame cap mapping metadata cleared in `performPageInvocationUnmap()`
5. VSpace teardown:
   - `deleteASID()`
   - `invalidateTLBByASID()`
   - `invalidateASIDEntry()` in hypervisor mode
   - clear ASID table entry to none
   - `setVMRoot()` on current thread
6. Untyped reset and later reissue:
   - `resetUntypedCap()`
   - `clearMemory()` only, not `clearMemory_PT()`
   - update free index
   - later `invokeUntyped_Retype()`
   - `createNewObjects()`
   - `Arch_createObject()`
   - page-table object entries set to `pte_pte_invalid_new()`
   - creation-time flush currently uses `cleanCacheRange_PoU()` for VSpace/PageTable objects

Allocator / reuse-model result:

1. The historical trigger tests run on a genuinely freeable allocator path.
2. On the sel4test side, `init_allocator()` uses `bootstrap_use_current_1level()`
   and then `allocman_utspace_add_uts()` to seed the allocator with the
   untypeds handed over by the driver.
3. On the driver side, `init_env()` uses `bootstrap_use_current_simple()`,
   which also ultimately creates the same split-based untyped backend.
4. `libsel4allocman/src/bootstrap.c` hardwires the bootstrap untyped manager to
   `utspace_split`, not `utspace_twinkle`.
5. `utspace_split` is explicitly freeable and buddy-coalescing:
   - `_utspace_split_alloc()` removes a node from the split tree and returns it
     as the allocation cookie
   - `_utspace_split_free()` reinserts that node and recursively coalesces with
     its sibling when both are free
6. The sel4utils shared-vspace frame path sits directly on that allocator:
   - `new_pages_at_vaddr()` allocates frames with `vka_alloc_frame_maybe_device()`
   - `sel4utils_unmap_pages()` later unmaps, deletes the cap, frees the slot,
     and finally returns the stored frame cookie via `vka_utspace_free()`
7. Missing paging structures are allocated on demand through the same broad VKA
   regime:
   - `sel4utils_map_page()` allocates missing page-table objects with
     `vka_alloc_object()`
   - the `vka` in these paths is allocman-backed
   - on AArch64, `vspace_get_map_obj()` allocates missing paging structures in
     response to `seL4_FailedLookup`, with the common smallest missing object
     being `seL4_ARM_PageTableObject`
8. On this AArch64 configuration, the relevant object sizes line up directly:
   - `seL4_PageBits == 12`
   - `seL4_PageTableBits == 12`
   - `seL4_ARM_SmallPageObject` therefore consumes the same 4 KiB untyped size
     as `seL4_ARM_PageTableObject`
   - the higher vspace-root object can differ in size under the 40-bit-PA
     hypervisor configuration, so the strongest same-size reuse claim is about
     4 KiB frame pages and 4 KiB page-table objects, not every level of the
     vspace hierarchy
9. Therefore the dominant historical trigger tests are not just causing logical
   map/unmap churn; they are exercising a real split-backed allocation/free/reuse
   cycle underneath both frame objects and paging-structure objects, with the
   key frame/page-table case also sharing the same object size.
10. The remaining proof gap is narrower:
   - source now strongly supports shared allocator pressure and same-size reuse
   - what is not yet fully proven is the exact frequency with which a freed
     4 KiB frame allocation is later repurposed as a page-table object under
     the dominant trigger workloads, as opposed to later reuse elsewhere in the
     same allocator regime

Trigger-loop reuse refinement:

1. The dominant helper-thread loops definitely churn 4 KiB frame objects:
   - one IPC-buffer page
   - `stack_size` 4 KiB stack pages
   - repeated unmap/delete/free back into allocman on teardown
2. They are a weaker fit for repeated page-table-object churn on every
   iteration:
   - sel4utils bookkeeping levels are not torn down by ordinary
     `vspace_free_sized_stack()` / `vspace_free_ipc_buffer()`
   - freed stack reservations reset `last_allocated`, making reuse of the same
     virtual-address window likely
   - once the corresponding kernel paging levels exist for that window, later
     helper-thread stack reuse may map only leaf frames and avoid fresh
     `seL4_FailedLookup` paging-object creation in the hot path
3. Therefore the strongest immediate workload-fit story is now:
   - heavy leaf-page map/unmap/free churn in one long-lived root
   - allocator-side 4 KiB frame reuse
   - delayed detector sites during unmap / walker-reader code
4. Same-size frame-to-page-table repurposing remains plausible in the broader
   allocator regime, but it is no longer the default assumption for every hot
   helper-thread iteration

Trigger-specific delayed-cleanup result:

1. `FPU0001` performs explicit helper cleanup inside the test loop:
   - `create_helper_thread()`
   - `start_helper()`
   - `wait_for_helper()`
   - `cleanup_helper()`
2. `CANCEL_BADGED_SENDS_0002` does not perform matching helper cleanup in the
   test body:
   - it creates 32 helpers and a reply thread
   - it allocates one sync endpoint plus two endpoint-derived caps per helper
   - it then repeatedly sweeps `cnode_revoke()` and
     `cnode_cancelBadgedSends()` across those caps
   - the test returns without per-helper `cleanup_helper()` calls
3. In the BASIC test harness, delayed cleanup then happens outside the test:
   - `basic_tear_down()` first unmaps the shared init frame
   - then revokes every handed-over untyped cap for the whole test process
   - then calls `sel4utils_destroy_process()`
4. `sel4utils_destroy_process()` in turn:
   - destroys the root thread via `sel4utils_clean_up_thread()`
   - tears down the process vspace via `vspace_tear_down()`
   - frees objects recorded by the vspace allocated-object callback
5. This makes `CANCEL_BADGED_SENDS_0002` a strong source-backed fit for the
   older “corruption earlier, detector later in vspace teardown” reading:
   - cap/MDB/endpoint pressure happens during the test body
   - the hot vspace/unmap detector sites can naturally occur later during
     BASIC teardown rather than at the moment of `cancelBadgedSends()`
6. The kernel-side teardown ordering strengthens that reading further:
   - BASIC teardown revokes the test process's handed-over untyped caps before
     calling `sel4utils_destroy_process()`
   - `vka_cnode_revoke()` reaches kernel `cteRevoke()`
   - `cteRevoke()` repeatedly drives `cteDelete()` over descendant caps
   - `cteDelete()` calls `finaliseSlot()`, which in turn uses
     `finaliseCap()`, zombie reduction if needed, `emptySlot()`, and
     `postCapDeletion()`
   - `cteRevoke()` and `finaliseSlot()` both contain preemption points, so this
     is not one atomic local action but a long lifecycle-heavy cleanup walk
7. That means `CANCEL_BADGED_SENDS_0002` is not just “endpoint revoke stress”.
   It is also a strong teardown-ordering stressor for:
   - descendant-cap deletion
   - MDB maintenance
   - final-cap decisions
   - post-cap-deletion hooks
   - later untyped reset and object reuse
8. On AArch64, the generic cap-deletion walk reaches the exact vspace hooks of
   interest through `Arch_finaliseCap()`:
   - `cap_asid_pool_cap` -> `deleteASIDPool()`
   - final `cap_vspace_cap` -> `deleteASID()`
   - final mapped `cap_page_table_cap` -> `unmapPageTable()`
   - mapped `cap_frame_cap` -> `unmapPage()`
   - `Arch_postCapDeletion()` itself is empty on this path
9. So the generic teardown walk and the hot vspace detector sites are not
   separate stories:
   - `cteRevoke()` / `cteDelete()` / `finaliseCap()` is the generic front half
   - `unmapPage()`, `unmapPageTable()`, and `deleteASID()` are the injected
     architecture-specific back half
10. The current source also exposes an important asymmetry inside that path:
    - generic cap deletion reaches `unmapPageTable()`, but does not call
      `clearMemory_PT()` there
    - generic cap deletion reaches `deleteASID()`, but `deleteASID()` only
      invalidates translations and clears the ASID mapping; it does not clear
      or republish the VSpace root memory itself
    - `Arch_postCapDeletion()` is empty, so there is no later ARM-specific
      cleanup hook repairing that gap
11. Therefore the generic final-cap deletion path mostly performs logical
    retirement of mappings and roots, while underlying object memory can remain
    to be cleaned only later by broader reuse/reset mechanisms.
12. The userspace-side destroy path mirrors that same staged shape:
    - `sel4utils_destroy_process()` does thread cleanup first
    - then `vspace_tear_down()`
    - then only afterwards frees objects accumulated via the vspace
      `allocated_object` callback in `clear_objects()`
13. Since missing paging structures are tracked through that callback, process
    destruction does not present one single retire-and-free moment for all
    paging-related state.
    - on the active Orin AArch64 hypervisor configuration with 40-bit PA,
      `vspace_get_map_obj()` feeds that path with `seL4_ARM_PageTableObject`
      and `seL4_ARM_PageDirectoryObject`; the `NO_PUD` case is compiled out
    - ordinary frame objects are not what populate this late free list
14. `sel4utils_tear_down()` recursively frees mapped pages and unmaps the
    sel4utils bookkeeping tree from the bootstrap vspace, but it does not
    consume the separate allocated-object list itself.
15. That is another concrete source-backed asymmetry matching the broader
    audit theme:
    - leaf-page teardown can happen first
    - paging-object cap/object free can lag behind
    - later detector sites can therefore sit in teardown code without being the
      original disturbance site
16. The late `clear_objects()` free-list walk does not create a separate kernel
    disposal path:
    - `vka_free_object()` performs `seL4_CNode_Delete` on each recorded paging
      object cap
    - then frees the cslot and utspace allocation
    - when the deleted cap is final, this re-enters the same kernel
      `cteDelete()` / `finaliseCap()` path already implicated elsewhere
17. So the userspace split teardown does not compete with the kernel final-cap
    story; it is an additional delayed feeder into it.
18. The phase ordering is explicit rather than inferred:
    - leaf-page teardown happens during `vspace_tear_down()`
    - only afterwards does `clear_objects()` begin deleting the recorded
      paging-object caps
19. That means one process-destruction path can naturally contain multiple
    delayed detector windows:
    - first at leaf-unmap / walker-reader sites
    - later again at final-cap deletion of PT/PD objects
20. This is not just an ordering guess:
    - `vspace_tear_down()` contains no `PageTable_Unmap` /
      `PageDirectory_Unmap` step for the recorded process paging objects
    - it frees leaves and bootstrap bookkeeping mappings instead
    - so the recorded PT/PD caps can remain to be retired later by
      `clear_objects()`
21. The late PT/PD free path is still weaker than the explicit page-table
    invocation-unmap path:
    - `Arch_finaliseCap()` will only call `unmapPageTable()` if the deleted
      page-table cap is still marked mapped
    - that final-cap path retires the parent entry, but it does not include the
      later `clearMemory_PT()` step used by `performPageTableInvocationUnmap()`
    - so late PT/PD deletion is a real delayed teardown window, but it is not
      the same full retire-and-clear sequence as explicit page-table unmap
22. The late VSpace-root free is an even smaller secondary window:
    - libsel4utils allocates one root object per process and assigns it to an
      ASID pool immediately during process configuration
    - there is no corresponding libsel4utils step that clears the root cap's
      mapped state before the final `vka_free_object(&process->pd)`
    - so that final root-cap delete can still feed `deleteASID()` through
      `Arch_finaliseCap()`
    - but it contributes at most one root-teardown event per destroyed
      process, making it a weaker fit to the historical hot-stack frequency
      than repeated leaf-frame teardown

Current highest-risk handoff assumptions:

1. Creation-time publication mismatch:
   - runtime mutation paths moved to conservative `PoC` maintenance
   - creation-time VSpace/PageTable paths still publish with `PoU`
   - boot-time parent-link publication still lacks adjacent maintenance, but
     boot later performs a coarse boot-region `PoC` flush before the initial
     thread runs
2. Reuse-chain split contract:
   - parent unlink + TLBI is done in one function
   - child-table clearing is done later in another function
   - untyped reset uses generic `clearMemory()`
   - later reissue relies on `Arch_createObject()` to fully reestablish a safe state
3. Map-path implicit assumption:
   - invalid-to-valid publication paths do not TLBI
   - this is now supported by Arm guidance stating that translations which
     fault are not permitted to be cached in TLBs, so first-time mapping does
     not require TLBI
   - TLBI remains required for unmap or for changing an existing valid mapping
4. Documentation drift:
   - some older notes describe stronger local barriers than the current code actually contains
   - current conclusions must therefore be derived from the checked-out tree plus primary external sources

Current ranking from the source audit so far:

1. Most plausible overall class:
   - lifecycle / reuse / publication contract weakness across multiple helpers
2. Strongest end-to-end runtime candidate:
   - shared-vspace leaf-page churn inside one long-lived root, coupled to
    split-backed 4 KiB frame free/reuse and later detector sites in unmap /
    walker-reader code
3. Strongest concrete local code target:
   - `resetUntypedCap()` plus later same-size 4 KiB reuse paths, because
     reused untyped memory still goes through plain `clearMemory()` before
     being repurposed as either a frame or a page-table object
4. Strong but broader consistency concern:
   - VSpace-root runtime publication/reuse with `PoU`
5. Lower-confidence but still relevant:
   - valid-to-valid leaf replacement ordering in `performPageInvocationMap()`,
     especially the cacheability-changing subset
6. Currently weaker:
   - implicit EL2 translation-regime / TLBI helper contracts that are hard to
     prove from the current helper boundaries
7. Detector-only interpretation that currently fits better than origin-site blame:
   - `findMapForASID()` / `isFinalCapability()` as later integrity readers
8. Important workload split to preserve in future reasoning:
   - `FPU0001` and `CANCEL_BADGED_SENDS_0002` likely stress different halves of
     the same broad problem, rather than one identical hot loop
9. Current trigger-specific weighting:
   - `FPU0001` is the cleaner fit for immediate shared-vspace 4 KiB frame churn
   - `CANCEL_BADGED_SENDS_0002` is the cleaner fit for delayed teardown walks
     through cap/MDB/untyped-reset lifecycles, with later vspace detector sites
10. Additional teardown-shape result:
   - process destruction itself is split between `vspace_tear_down()` and the
     later `clear_objects()` free-list walk
   - that further supports treating teardown hot PCs as detector/transition
     sites rather than as proof that the final freeing site is the origin

Refined runtime-reuse focus:

1. Among the lifecycle candidates, VSpace-root reuse is currently more
   suspicious than ordinary page-table-object reuse because page-table objects
   at least have an explicit `clearMemory_PT()` step on the invocation-unmap
   path, while VSpace finalisation does not.
2. This also further weakens `findMapForASID()` as an origin hypothesis:
   once `deleteASID()` has run, logical ASID reachability is gone, so later
   `findMapForASID()` hits are more naturally interpreted as checks over newly
   republished state than as reads of a still-live stale root.

Creation-path history result:

1. Safe invalid initialization for newly created `VSpace` / `PageTable`
   objects was added later than the original creation-time flush logic.
2. The current tree therefore combines:
   - newer safe-invalid entry contents
   - older `cleanCacheRange_PoU()` creation-time publication
3. This makes the creation path look like a partial modernization rather than a
   fully reconsidered end-to-end contract, and it strengthens the concern that
   child-table contents may not be published with the same visibility model as
   the later parent-entry publication that makes them reachable.

Boot-sequence correction:

1. The boot builder helpers themselves still do not perform adjacent cache
   maintenance on each parent-link write.
2. However, `kernel/src/arch/arm/kernel/boot.c` performs a later coarse flush
   before the initial thread is created, specifically to make page-table
   information visible to the hardware walker.
3. On current AArch64, `cleanInvalidateL1Caches()` is misleadingly named:
   it delegates to `cleanInvalidate_D_PoC()` over the boot region, not to a
   literal L1-only operation.
4. Therefore:
   - boot-only missing local flushes are weaker than first assumed
   - the stronger remaining creation-path concern is for runtime object
     creation/reuse, where `Arch_createObject()` still uses `PoU` for new
     VSpace/PageTable objects

Current unifying hypothesis from source alone:

1. The remaining Orin RAS family may be best explained not by one isolated
   barrier omission, but by mixed-generation visibility of translation
   metadata.
2. In that model:
   - child-table contents are initialized to safe invalid values
   - their visibility contract is weaker or less explicit than the parent-entry
     publication that later exposes the child table to walkers
   - later lifecycle operations may again split unlink, clearing, reuse, and
     reissue across multiple helpers
3. This naturally fits the observed pattern where low-level readers such as
   `pte_ptr_get_pte_type()`, `findMapForASID()`, and `isFinalCapability()` look
   more like later integrity checkpoints than origin sites.

## Interim Verdict

The source audit now supports a more specific position than the initial
"speculative PTW vs something else" framing.

### Current best explanation

The strongest current explanation is:

1. seL4 is likely republishing translation metadata under inconsistent
   visibility and lifetime contracts across different stages of the object
   lifecycle;
2. speculative PTW remains a credible immediate consumer of that bad state in
   some cases, but it no longer looks like the whole root-cause story;
3. the remaining open Orin RAS family is more plausibly driven by stale or
   mixed-generation translation metadata being made reachable again than by one
   isolated EL2 barrier omission.
4. the best current *detector-path* ranking is no longer broad or symmetric:
   - first: frame-cap finalisation / leaf unmap via `unmapPage()`
   - second: `lookupPTSlot()` / `pte_ptr_get_pte_type()` as the immediate
     page-table walk/read site
   - lower-frequency but still relevant: `unmapPageTable()` and `deleteASID()`
5. the best current *workload-fit* split is:
   - `FPU0001` as immediate shared-vspace 4 KiB frame churn
   - `CANCEL_BADGED_SENDS_0002` as delayed teardown stress through
     `cteRevoke()` / `cteDelete()` / `Arch_finaliseCap()` and later
     vspace/untyped cleanup
6. the userspace-side process-destruction path reinforces that split-teardown
   interpretation:
   - `sel4utils_destroy_process()` does not free all paging-related state in
     one step
   - it runs `vspace_tear_down()` and only then frees vspace-recorded paging
     objects through `clear_objects()`
   - on Orin's active AArch64+EL2+40-bit configuration, that late free list is
     specifically page-directory/page-table objects created on `FailedLookup`,
     not a broad generic object mixture
   - those late frees still use ordinary `seL4_CNode_Delete` on the object
     caps, so they naturally re-enter the same kernel final-cap machinery
     rather than creating a separate disposal path
   - because `vspace_tear_down()` does not itself unmap those recorded PT/PD
     objects, the later `clear_objects()` walk is the point where their cap
     retirement actually starts
   - that gives one process-destruction path multiple delayed detector windows:
     leaf-frame teardown first, then PT/PD final-cap teardown later
   - the PT/PD window still looks secondary to the leaf window:
     if it reaches `unmapPageTable()`, it still lacks the explicit
     `clearMemory_PT()` step seen in invocation-driven page-table unmap
   - the final root-cap free after that looks weaker still:
     it can feed `deleteASID()`, but only once per destroyed process
   - this makes delayed detector hits during teardown even easier to explain
     without treating the final free site as the origin

The current best workload fit for the historically dominant sel4test triggers
is now narrower than that:

1. helper-thread stack / IPC-buffer / paging-structure churn inside one
   long-lived shared `env->vspace`
2. immediate return of freed frame cookies into a split-backed freeable untyped
   allocator
3. likely reuse of the same virtual-address windows for those helper-thread
   stack / IPC allocations
4. heavy repeated leaf-page map/unmap/free churn without needing fresh
   page-table allocation on every iteration
5. later untyped reset with plain `clearMemory()`
6. later reuse of that allocator pool by new 4 KiB frame or 4 KiB page-table
   objects elsewhere in the same allocator regime
7. eventual detection at cleanup/walker-reader sites such as `unmapPage()`,
   `lookupPTSlot()`, and `pte_ptr_get_pte_type()`

There is now an important trigger-specific qualifier on top of that model:

1. `FPU0001` fits the immediate helper-thread churn path directly.
2. `CANCEL_BADGED_SENDS_0002` fits a delayed detector path:
   - endpoint/MDB activity during the test body
   - whole-process BASIC teardown later
   - untyped revoke plus process/vspace destruction after the test returns
3. In kernel terms, that delayed path explicitly includes:
   - `cteRevoke()`
   - `cteDelete()`
   - `finaliseCap()`
   - `Arch_finaliseCap()` dispatch to `unmapPage()`, `unmapPageTable()`,
     `deleteASID()`, and `deleteASIDPool()` as applicable
   - `postCapDeletion()`
   - later `resetUntypedCap()` once children are gone

Arch-hook injection result:

1. The generic CNode teardown path and the AArch64 vspace teardown path are
   directly connected in current source.
2. For mapped frame/page-table/vspace-related caps, the generic
   `cteDelete()`/`finaliseSlot()` machinery does not merely lead to later
   cleanup by convention; it calls into `Arch_finaliseCap()`, which directly
   invokes:
   - `unmapPage()`
   - `unmapPageTable()`
   - `deleteASID()`
   - `deleteASIDPool()`
3. This strengthens the interpretation of hot PCs in `unmapPage()` and
   `pte_ptr_get_pte_type()` for teardown-heavy workloads:
   - they can be natural detector/transition sites reached from broad cap/MDB
     teardown, not just from user-visible page-unmap invocations
4. It also weakens any attempt to separate “endpoint/cnode tests” from
   “vspace teardown bugs” too sharply:
   - in the BASIC teardown path, cap deletion and vspace teardown are part of
     one connected lifecycle chain
5. It sharpens the reuse/lifetime concern inside that same chain:
   - frame-cap finalisation calls `unmapPage()`
   - page-table-cap finalisation calls `unmapPageTable()` but not
     `clearMemory_PT()`
   - vspace-cap finalisation calls `deleteASID()` but does not sanitize the
     root page-table memory
   - so the generic delete path retires reachability first and leaves actual
     memory sanitisation to later phases such as explicit unmap invocations or
     eventual untyped reset
6. The userspace-side destroy path mirrors that split lifecycle:
   - leaf-frame unmap/free happens inside `vspace_tear_down()`
   - paging objects collected via `allocated_object` are only freed later by
     `clear_objects()`
   - so even above the kernel boundary, the dominant teardown path is not one
     symmetric "retire and sanitize everything now" action

Finalisation-path asymmetry result:

1. The current tree does not apply one symmetric “retire and sanitize now”
   policy across frame, page-table, and vspace-root teardown.
2. In particular:
   - explicit page-table unmap invocation later does call `clearMemory_PT()`
   - generic final-cap deletion of a page-table cap does not
   - generic final-cap deletion of a vspace cap does not clear the root table
     memory at all
3. This is a strong source-only fit for delayed-detector behavior:
   - the deletion path can make an object logically unreachable
   - but underlying translation metadata may persist until later reset/reuse
4. That strengthens the current ranking toward:
   - lifecycle / reuse / publication weakness
   - delayed teardown detection
   - later `resetUntypedCap()` / retype as the point where stale underlying
     memory finally matters again

Primary detector-path weighting result:

1. The older trigger-specific notes consistently weight `unmapPage()` above
   `unmapPageTable()` and `deleteASID()` as the dominant observed detector path
   in teardown-heavy runs.
2. In particular:
   - the strongest `CANCEL_BADGED_SENDS_0002` ftrace evidence lands in
     `unmapPage() -> lookupPTSlot() -> pte_ptr_get_pte_type()`
   - the older FPU/thread-lifecycle notes also trace cleanup through
     `cleanup_helper() -> sel4utils_clean_up_thread() -> vspace_free_sized_stack()
     -> seL4_ARCH_Page_Unmap -> unmapPage()`
   - the broader investigation log explicitly notes `unmapPageTable` as
     frequently absent from some relevant windows
3. So while page-table-cap and vspace-cap finalisation remain important to the
   lifetime model, the best current source-and-docs ranking for *detector sites*
   is:
   - first: frame-cap finalisation / leaf unmap (`unmapPage`)
   - second: page-table walks under `lookupPTSlot` / `pte_ptr_get_pte_type`
   - lower-frequency but still relevant: page-table-cap finalisation
     (`unmapPageTable`) and ASID-root finalisation (`deleteASID`)
4. The newer userspace-destruction analysis does not overturn that ranking:
   - late PT/PD frees from `clear_objects()` can feed `unmapPageTable()`
   - but that path is conditional on the cap still being marked mapped
   - and unlike explicit page-table invocation unmap, it does not perform the
     follow-up `clearMemory_PT()` step
   - so it remains a secondary delayed detector window rather than the best
     primary detector fit
5. The corresponding late root-delete window looks even weaker:
   - final process root deletion can still feed `deleteASID()`
   - but it happens once per destroyed process rather than once per torn-down
     frame mapping, so it is an even poorer fit to the observed hot-stack
     frequency than `unmapPageTable()`
6. This fits the current workload split:
   - `FPU0001` directly creates lots of helper-thread stack / IPC-buffer frame
     teardown
   - `CANCEL_BADGED_SENDS_0002` reaches similar detector sites later during
     whole-process teardown

Historical-note drift result:

1. The older investigation log also records a previous attempted fix that
   cleared VSpace and page-table entries during `Arch_finaliseCap()`.
2. That logic is not present in the current checked-out tree.
3. Therefore those notes are now useful as:
   - evidence of what was previously considered a plausible mitigation
   - evidence that the current tree has drifted away from that mitigation
4. They are not proof that the current tree still contains the exact same code
   or fix state, so current source remains authoritative when there is conflict.

### Current ranking, tightened

1. Most plausible overall class:
   - lifecycle / reuse / publication contract weakness across multiple helpers
2. Primary observed detector path:
   - `unmapPage() -> lookupPTSlot() -> pte_ptr_get_pte_type()`
3. Strongest workload-fit runtime model:
   - shared-vspace leaf-page churn inside one long-lived root, coupled to
     split-backed 4 KiB frame free/reuse and later teardown detection
4. Strongest concrete cleanup/reuse target:
   - `resetUntypedCap()` plus later same-size 4 KiB reuse paths, because
     reused untyped memory still goes through plain `clearMemory()`
5. Strongest current *origin-side* model:
   - stale translation state is most plausibly introduced at the generic
     reset/reissue boundary, where:
     - `resetUntypedCap()` clears reused memory with plain `clearMemory()`
     - reissued `VSpace` / `PageTable` objects are then republished from
       `Arch_createObject()` with `cleanCacheRange_PoU()`
     - only the explicit invocation-driven page-table unmap path applies the
       stronger `clearMemory_PT()` step to child table memory
   - the implementation difference is concrete in source:
     - `clearMemory()` is just `memzero()`
     - `clearMemory_PT()` is `memzero()` plus
       `cleanInvalidateCacheRange_RAM()` (`dc civac` / PoC-style retire)
     - `cleanCacheRange_PoU()` uses `dc cvau`
   - so the current translation-object lifecycle really crosses three
     different memory-publication contracts, not one uniform contract
   - local Linux arm64 source strengthens the concern with a direct analogue:
     - after allocating a zeroed page-table page,
       `arch/arm64/mm/mmu.c::__pgd_pgtable_alloc()` issues `dsb(ishst)`
     - the comment there is explicit: "Ensure the zeroed page is visible to
       the page table walker"
     - that is not final proof against seL4, but it is a strong local
       upstream signal that fresh table publication wants an explicit
       walker-visibility step
6. Strongest broader structural asymmetry:
   - generic final-cap deletion retires reachability sooner than it sanitizes
     underlying page-table/vspace object memory
7. Important but now secondary:
   - runtime `VSpace`-root publication/reuse with `PoU`
8. Still relevant but lower-confidence:
   - valid-to-valid leaf replacement ordering in `performPageInvocationMap()`,
     especially the cacheability-changing subset
9. Weaker at present:
   - implicit EL2 translation-regime / TLBI helper contracts as the dominant
     explanation
10. Best interpreted as detector sites, not origins:
   - `findMapForASID()` and `isFinalCapability()`

### Why this is stronger than the earlier framing

1. Current EL2 helper code does not show a clean single missing-barrier
   smoking gun.
2. `findMapForASID()` and `isFinalCapability()` look increasingly like detector
   sites, not origin sites.
3. The trigger-specific source paths now fit delayed teardown and leaf-page
   cleanup better than they fit a pure “fresh VSpace root publication” story.
4. Runtime creation/reuse of `VSpace` roots is still a confirmed current path
   that republishes new roots with `PoU`, so it remains an important secondary
   structural concern.
5. `performPageInvocationMap()` still contains a legacy valid-to-valid remap
   sequence that deserves scrutiny, but it is best viewed as one concrete
   manifestation inside a broader lifecycle/publication problem family.

### What this means for commit `eba5a3b6...`

The current source-only verdict on
`eba5a3b6e6b804a77c6cfc8d2a6535707c953c9a` is:

1. the code change is likely helpful and should stay for now;
2. the explanation should be rewritten to say that `PoC` is a conservative
   implementation choice for Orin-class systems, not a universal architectural
   statement about PTW;
3. the commit should be treated as the runtime-mutation slice of a larger
   cleanup, not as the definitive page-table coherency fix;
4. specifically, it does not address the strongest current source-backed issues:
   - delayed teardown asymmetry in generic final-cap deletion
   - later untyped reset with plain `clearMemory()`
   - trigger-specific teardown paths that reach `unmapPage()` as a detector

### Commit rewrite direction

If the rationale around `eba5a3b6...` is revised later, the safer shape is:

1. claim only that `PoC` is the conservative visibility target chosen for
   Orin-class systems and mixed-cache hierarchies;
2. avoid claiming that the architecture simply defines PTW as a `PoC`
   consumer;
3. state explicitly that runtime mutation was cleaned up here, while creation,
   reuse, and remap ordering require separate review;
4. avoid implying that converting `PoU` to `PoC` alone closes the remaining
   RAS family.

### Remaining proof gaps

1. A fully explicit primary Arm-source-backed answer is still wanted for the
   exact valid-to-valid leaf remap subcase in `performPageInvocationMap()`.
2. Arm's own public guidance already narrows the gap:
   - TLB invalidation is required when changing a mapping's output address or
     any attributes
   - pre-Armv8.4 break-before-make is explicitly required for changes to
     memory type, cacheability, output address, block/page size, and some
     global/non-global transitions
   seL4's exact remap surface is now narrowed:
   - the `armPageCacheable` subset falls directly into the explicit
     BBM-sensitive bucket
   - the remaining gap is the rights / executability-only subset
3. For runtime `VSpace` creation/reuse, the source mismatch is already
   confirmed; the remaining gap is to classify it more sharply as either:
   - merely conservative-debt, or
   - a direct contract violation for Orin-class systems

### Next evidence to seek

The next highest-value evidence would be one of:

1. a primary architectural source that clearly classifies the remaining
   permission-only / executability-only subset of valid-to-valid leaf
   replacement as requiring, or not requiring, a BBM-style break phase; or
2. a stronger primary-source argument that runtime publication of fresh
   translation tables must reach the same visibility point as the parent entry
   that exposes them to the walker.

### Current one-paragraph conclusion

The best current source-only reading is that Orin's remaining seL4 RAS family
is primarily a lifecycle/reuse/publication problem, not a single EL2 barrier
mistake. The dominant observed detector path is leaf-page teardown,
specifically `unmapPage() -> lookupPTSlot() -> pte_ptr_get_pte_type()`. The
dominant trigger tests likely stress different halves of the same family:
`FPU0001` through immediate shared-vspace 4 KiB frame churn, and
`CANCEL_BADGED_SENDS_0002` through delayed `cteRevoke()` / `cteDelete()` /
`Arch_finaliseCap()` teardown walks that later enter the same vspace detector
sites. The strongest current origin-side suspicion is the reset/reissue
boundary for translation objects, where `resetUntypedCap()` uses plain
`clearMemory()`, later `Arch_createObject()` republishes translation objects
with `PoU`, and only explicit invocation-driven page-table unmap applies
`clearMemory_PT()`. In concrete implementation terms, that means bare zeroing,
`dc cvau` publication, and `dc civac` retirement are all mixed across one
translation-object lifecycle. `eba5a3b6...` remains directionally useful as conservative
mutation-side `PoC` cleanup, but it should be described as partial, because the
current tree still treats logical retirement more consistently than underlying
translation-
metadata sanitisation and later reuse.

## Implications For New RAS Runs

The current audit changes how new Orin RAS runs should be read.

### How to interpret hot PCs now

Given the current source-backed ranking:

1. `pte_ptr_get_pte_type()`
   - still fits as a direct consumer of bad translation metadata
   - but should not be assumed to be the origin of corruption
2. `unmapPage()`
   - remains a meaningful transition site because it retires live entries and
     participates in the reuse chain
   - however, seeing it in the stack is no longer enough to conclude the bug is
     only in local unmap ordering
3. `findMapForASID()`
   - is best read as an integrity check over currently published ASID/vspace
     state
   - repeated hits here are more consistent with stale or wrongly republished
     roots than with a still-live deleted root
4. `isFinalCapability()`
   - is best read as a later metadata consumer in a lifecycle-heavy window
   - it does not currently look like an architectural root cause by itself

### What a new run would mean under the current model

If a new run again clusters in:

1. `unmapPage()` plus `pte_ptr_get_pte_type()`
   - that is consistent with transition consumers observing stale translation
     metadata
2. `findMapForASID()` plus later vspace/PT readers
   - that strengthens the republished-root / reused-table interpretation
3. `isFinalCapability()` plus cap-finalisation paths
   - that strengthens the broader lifecycle-window interpretation, not a pure
     PTW-only story

### What would materially weaken the current model

The current leading explanation would weaken if source or primary external
evidence showed either:

1. runtime `VSpace` publication with `PoU` is architecturally sufficient even
   for Orin-class systems in the exact way seL4 uses it; and
2. valid-to-valid leaf replacement in `performPageInvocationMap()` is
   explicitly acceptable for the permissions/attribute changes seL4 allows
   there.

Without both of those, the current lifecycle/publication ranking remains the
stronger reading.

### What not to overinfer yet

The current audit is strong enough to rank explanations, but not yet strong
enough to claim:

1. one exact ARM architectural rule has already been proven violated by the
   runtime `VSpace` creation path; or
2. `performPageInvocationMap()` has already been proven architecturally invalid
   for every remap it can perform.

The right current statement is narrower:

1. the runtime `VSpace` publication path is the clearest confirmed source-level
   contract mismatch; and
2. the valid-to-valid remap path is the clearest single-function sequence that
   still needs a stronger primary-source judgement.

## Current Conclusion

If work stopped here, the source-only conclusion would be:

1. The current open Orin RAS family is best explained by translation-metadata
   lifecycle and publication weaknesses, not by a single isolated EL2 barrier
   mistake and not by treating every hot PC as an origin site.
2. For the historically dominant sel4test trigger tests, the better current
   fit is shared-vspace page/frame/page-table lifecycle churn inside one
   long-lived root, not repeated fresh top-level `VSpace`-root publication.
   - the teardown side of that fit is now stronger because both kernel
     final-cap deletion and userspace process destruction retire different
     layers at different times rather than collapsing all paging-related state
     into one symmetric free point
3. The fresh `VSpace`-root publication path through runtime
   retype/create/ASID-assign still relying on `PoU` remains a confirmed source
   mismatch, but it is no longer the best workload match for
   `CANCEL_BADGED_SENDS_0002` and `FPU0001`.
4. The most plausible *origin* boundary is now narrower than the detector
   model:
   - generic reset/reuse through `resetUntypedCap()` leaves reused memory with
     only plain `clearMemory()`
   - later `Arch_createObject()` republishes new `VSpace` / `PageTable`
     objects with `cleanCacheRange_PoU()`
   - only explicit invocation-driven page-table unmap applies
     `clearMemory_PT()`
   - those are not just naming differences:
     `clearMemory()` is bare zeroing, `clearMemory_PT()` adds
     `cleanInvalidateCacheRange_RAM()` (`dc civac`), and creation-time
     publication uses `cleanCacheRange_PoU()` (`dc cvau`)
   - so the strongest current origin-side suspicion is the reset/reissue
     boundary for translation objects, not the later teardown readers that
     detect the damage
5. The clearest single-function sequence still demanding a stronger
   architecture/upstream judgement is valid-to-valid leaf replacement in
   `performPageInvocationMap()`, but the strongest subcase there now appears to
   be relatively specialised rather than clearly hot.
6. Commit `eba5a3b6...` should currently be read as useful but partial:
   it improves runtime mutation visibility, but it does not settle creation,
   reuse, or remap ordering.
7. Earlier sel4test clustering is strong enough to steer investigation
   priority:
   - it weakens explanations that depend on broad generic MMU traffic being
     sufficient on their own
   - it strengthens explanations tied to the specific lifecycle and wake/revoke
     patterns exercised by `CANCEL_BADGED_SENDS_0002` and `FPU0001`

## Next Proof Obligations

To move from a strong ranked explanation to a firmer conclusion, the next proof
obligations are:

1. Decide whether runtime publication of fresh `VSpace` roots with `PoU` is
   merely conservative debt or a direct contract violation for Orin-class
   systems.
2. Decide whether valid-to-valid remap for rights/cacheability/executability
   changes in `performPageInvocationMap()` requires a BBM-style break phase.
3. If both remain unproven, keep the current ranking but avoid claiming a fully
   settled architectural root cause.

## Implementation-Shaping Implication

If later kernel changes are made based on this audit, the safest decomposition
would be:

1. creation/publication contract cleanup for fresh `VSpace` / `PageTable`
   objects
2. reuse/retirement contract cleanup for root/table lifecycle transitions
3. separate remap-ordering review for valid-to-valid leaf replacement

That decomposition matches the current source evidence better than a single
"page-table coherency fix" narrative.

External-source result recorded:

1. Arm's memory-management guide states that the processor is not permitted to
   cache a translation that results in a translation fault, address size fault,
   or access-flag fault, and explicitly concludes that no TLB invalidate is
   needed when mapping an address for the first time.
2. That means the current lack of TLBI on invalid-to-valid publication paths is
   not, by itself, a strong bug candidate.
3. This pushes W2 suspicion further toward:
   - creation-time visibility choice (`PoU` vs conservative `PoC`)
   - split reuse-chain assumptions
   - unmap/change-existing-mapping retirement ordering

New upstream analogue recorded:

1. Linux arm64 `arch/arm64/mm/mmu.c` makes newly allocated zeroed page-table
   pages visible to the page-table walker explicitly before publication.
2. Linux arm64 `arch/arm64/include/asm/pgtable.h` and
   `Documentation/vm/arch_pgtable_helpers.rst` narrow live valid-to-valid
   updates to a small helper category:
   - `ptep_set_access_flags()` is only for changes to a more permissive PTE
   - generic arm64 comments describe only certain permission-attribute changes
     as safe without BBM
3. Linux KVM stage-2 `arch/arm64/kvm/hyp/pgtable.c` uses explicit
   break-before-make retirement with TLB maintenance for broader valid-entry
   replacement, and skips that only when the change is effectively "same
   mapping" or "permissions only" under its own narrow logic.
4. This does not by itself prove seL4 is wrong, but it materially strengthens
   two audit inferences:
   - fresh translation-table publication deserves an explicit walker-visibility
     argument, not a generic assumption that `PoU` is enough everywhere
   - seL4 `performPageInvocationMap()` is no longer just locally suspicious;
     it is now weaker than the nearest upstream analogue for broader mapping
     replacement

New primary Arm-source result recorded:

1. Arm's public memory-management guide states that TLB invalidation is needed
   when changing a mapping's output address or any attributes, with
   read-only-to-read-write given as an explicit example.
2. Arm System Memory documentation states that before Armv8.4-A,
   break-before-make is required for:
   - memory type changes
   - cacheability changes
   - output-address changes
   - block/page size changes
   - creating a global entry that can conflict with overlapping non-global
     TLB entries
3. These sources do not yet prove that every permission-only live remap needs
   BBM.
4. They do, however, materially strengthen the case against seL4
   `performPageInvocationMap()` as currently written, because that path can
   change `vm_attributes` as well as rights, and the attribute-changing subset
   now sits inside or adjacent to categories for which Arm already demands at
   least TLBI and, for several important attribute classes, BBM.

seL4 remap-surface classification recorded:

1. In AArch64 with hypervisor support, `makeUserPagePTE()` constructs a
   stage-2 leaf descriptor from:
   - `vm_rights_t` -> stage-2 access-permission bits
   - `armExecuteNever` -> execute-never bit
   - `armPageCacheable` -> stage-2 memory type plus shareability choice
2. Therefore a live remap through `performPageInvocationMap()` can cover:
   - permission-only change
   - executability-only change
   - cacheability/memory-type/shareability change
   - combinations of the above
3. The ABI bit `armParityEnabled` is not consumed by the current AArch64
   `makeUserPagePTE()` path, so it is not part of the live-remap concern here.
4. This sharpens the current verdict:
   - the `armPageCacheable`-changing subset is now a much stronger suspect,
     because it aligns directly with Arm's explicit BBM-sensitive categories
   - the unresolved subset is narrower: rights-only and possibly
     executability-only remap without cacheability change
   - the executability-only subset should not be judged in isolation from
     seL4's explicit instruction-cache flush API and its documented contract
   - as a result, the cleanest remap concern is now cacheability change first,
     rights-only second, and executability-only last

Workload-relevance note recorded:

1. Common user-level helper layers and tests predominantly map with
   `seL4_ARM_Default_VMAttributes`.
2. Uncached / non-default Arm mappings are present in the tree:
   - VM support code maps some DMA pages uncached
   - CapDL loader supports uncached mappings and explicitly performs
     cache-maintenance follow-up for uncached and executable cases
3. However, the source scan did not show strong evidence that
   same-frame/same-VA remap with changed cacheability is a dominant hot path in
   the observed Orin workloads.
4. Therefore the remap issue remains real and worth tracking, but current
   workload fit is weaker than the end-to-end `VSpace` publication/reuse path.

sel4test-trigger weighting recorded:

1. Older sel4test evidence in the Orin investigation log and legacy summary
   reports that only a small subset of tests triggered RAS at all, with the
   dominant share concentrated in `CANCEL_BADGED_SENDS_0002` and `FPU0001`.
2. Those trigger tests have different surface stories but overlap in the
   source patterns they stress:
   - rapid thread wake/resume cascades
   - heavy revoke/cleanup / later unmap activity
   - repeated object creation and page mapping in the setup path
3. This does not prove a single root cause, but it is strong weighting
   evidence against "generic background MMU traffic is enough" explanations.
4. It is also strong weighting evidence in favour of:
   - lifecycle/publication/reuse weakness
   - corruption during an earlier hot phase with later detection in
     `unmapPage()` / `lookupPTSlot()` / `pte_ptr_get_pte_type()`
   - trigger-specific hot paths being more useful audit targets than globally
     possible but apparently rare remap subcases
5. Therefore the earlier sel4test observations are sufficient to steer
   investigation priorities, even though they are not sufficient on their own
   to settle the root cause.

Trigger-test path narrowing recorded:

1. `FPU0001` hot loops create helper threads with `create_helper_thread()`,
   then destroy them with `cleanup_helper()`.
2. `CANCEL_BADGED_SENDS_0002` likewise creates many helper threads and a reply
   thread, then repeatedly exercises revoke/cancel/wake logic before helper
   cleanup.
3. For helper threads, the shared-state path is:
   - `sel4utils_configure_thread_config()` allocates TCB + IPC buffer + stack
     inside the existing `env->vspace`
   - `sel4utils_clean_up_thread()` frees TCB + IPC buffer + stack from that
     same vspace
   - stack pages go through `vspace_new_sized_stack()` /
     `vspace_free_sized_stack()`
   - the underlying libsel4utils shared-vspace allocator allocates frames,
     maps pages, unmaps pages, and frees backing untyped/frame state
4. This means the test-trigger evidence currently points more directly at:
   - page/frame/page-table lifecycle inside a long-lived shared vspace root
   - later detector sites reached during cleanup
5. It points less directly at:
   - repeated fresh top-level `VSpace` root creation as the dominant mechanism
     for the main historical trigger tests

Shared-vspace teardown chain recorded:

1. Helper-thread stack teardown flows through:
   - `vspace_free_sized_stack()`
   - `sel4utils_unmap_pages()`
   - `seL4_ARM_Page_Unmap`
   - kernel `performPageInvocationUnmap()` / `unmapPage()`
2. After unmap, userspace support code deletes the mapping cap and returns the
   backing frame allocation to allocman/vka untyped management.
3. Later reuse is mediated by kernel untyped retype:
   - `invokeUntyped_Retype()`
   - `resetUntypedCap()`
   - `createNewObjects()` / `Arch_createObject()`
4. The critical source fact is that `resetUntypedCap()` still clears reused
   untyped memory with plain `clearMemory()`, not `clearMemory_PT()`.
5. Combined with the trigger-test workload shape, this makes the most relevant
   lifecycle chain for historical sel4test RAS triggers:
   - shared-vspace page unmap
   - cap / frame free
   - later untyped reset
   - later object/frame/page-table reuse
6. This is now a better direct fit to the dominant trigger tests than the
   previously emphasised fresh-top-level-`VSpace` publication path.

New strongest local code candidate:

1. `performPageInvocationMap()` currently handles replacement of an already
   valid leaf entry as:
   - detect `tlbflush_required`
   - store new PTE
   - clean the new PTE
   - then invalidate by ASID+VA
2. That order is materially weaker than the stronger BBM-style model already
   captured elsewhere in the Orin notes:
   - break to safe invalid
   - publish
   - TLBI and wait
   - then make the new mapping
3. This does not prove it is the root cause of the observed RAS family, but it
   is currently the clearest source-level example of a transition sequence that
   deserves direct architectural scrutiny rather than assumption.
4. The analogous page-table parent publication path is narrower and less
   suspicious because decode rejects mapping into an already-valid parent slot;
   the local concern is therefore concentrated on valid-to-valid leaf
   replacement, not first-time table publication itself.
5. `decodeARMFrameInvocation()` constrains remap to the same frame, same ASID,
   and same virtual address, but it still permits changes to permissions and
   memory attributes through `vmRights` and `vm_attributes`.
6. That means `performPageInvocationMap()` is a real valid-to-valid descriptor
   replacement path for rights/cacheability/executability changes, not merely a
   redundant rewrite of an identical leaf entry.
7. History check: this ordering long predates the Orin work.
   - the store-then-TLBI structure comes from older seL4 code
   - `eba5a3b6...` only changed the cache-maintenance primitive from `PoU` to
     conservative `PoC`
   - if this path is wrong for Orin, it is best described as a legacy ordering
     assumption that was never revisited, not as a new Orin-specific regression
8. Current status of this candidate:
   - strongly suspicious
   - stronger than before because upstream analogues only permit no-BBM live
     updates in narrower cases than seL4's current remap path appears to cover
   - stronger again because Arm public guidance already says attribute changes
     require TLBI and several attribute classes require BBM before Armv8.4-A
   - the cacheability-changing subset is now close to a source-backed
     architectural indictment, not just a generic suspicion
   - not yet proven architecturally wrong from primary Arm sources for the
     remaining permission-only subset
   - executability-only remap still needs to be read together with seL4's
     explicit instruction-cache-flush contract, so it is no longer the cleanest
     unresolved part of this sequence
   - important enough to keep as a top audit target because it conflicts with
     the stronger BBM-style model already distilled elsewhere in the local Orin
     notes

Executable-publication note recorded:

1. seL4 exposes `ARMPageUnify_Instruction` and `ARMVSpaceUnify_Instruction`
   as first-class API operations.
2. The AArch64 implementation performs executable publication work there:
   - clean data to PoU
   - `dsb`
   - invalidate instruction cache to PoU
   - `isb`
3. `performPageInvocationMap()` itself does not perform that sequence on an
   `armExecuteNever -> executable` transition.
4. This suggests the kernel design expects executable visibility to be handled
   through an explicit caller-visible cache-maintenance contract, not solely as
   a side effect of mapping.
5. Therefore `NX -> X` is currently a weaker fit for the Orin RAS family than
   cacheability-changing remap or translation-metadata publication/reuse.

Runtime VSpace publication note from current cross-check:

1. Linux arm64 explicitly documents walker visibility when allocating a new
   zeroed page-table page.
2. seL4 runtime `VSpace` root creation still relies on
   `cleanCacheRange_PoU()` in `Arch_createObject()` and does not gain an extra
   publication step at ASID assignment time.
3. That does not yet prove a direct architecture violation, but it weakens the
   "probably just conservative debt" reading and strengthens the current
   ranking of runtime `VSpace`-root publication/reuse as the cleanest confirmed
   source-level mismatch.

Document-hygiene note from current cross-check:

1. Some older local audit/investigation notes describe stronger helper bodies
   than are present in the current source tree.
2. In particular, notes describing explicit pre-store `dsb()` in unmap paths or
   temporary Stage-2 disablement inside `setCurrentUserVSpaceRoot()` do not
   match the current checked-out code.
3. For this audit, the current kernel source is authoritative and older notes
   should be used only as hypothesis history unless they are revalidated
   against the tree.
