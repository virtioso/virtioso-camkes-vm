# Orin AArch64 EL2 RAS Implementation Tracker

## Summary

This tracker records the implementation progress for the kernel-side lifecycle
fix of the remaining Orin AArch64 EL2 RAS family.

Working model:

1. strongest origin-side issue: reset/reissue of translation objects
2. strongest detector path: `unmapPage() -> lookupPTSlot() -> pte_ptr_get_pte_type()`
3. secondary detector windows: late PT/PD final-cap teardown, then root-cap
   `deleteASID()`

Constraint:

1. all kernel-tree implementation steps are committed as logical git commits
2. docs tracking is kept on disk, but the docs repo is currently not in a
   commit-safe state

## Progress

Status values:

1. `planned`
2. `in_progress`
3. `done`
4. `blocked`

| Step | Status | Description | Acceptance | Commit SHA |
|---|---|---|---|---|
| K1 | done | Add dedicated AArch64 translation-object sanitisation/publication helper and use it for `seL4_ARM_VSpaceObject` / `seL4_ARM_PageTableObject` creation | New translation objects no longer use creation-time `PoU`-only publication | `aefb6dbf6` |
| K2 | done | Extend AArch64 final-cap teardown to sanitise PT and VSpace translation memory in the same arch path | Final PT/VSpace deletion now sanitises underlying translation memory after reachability retirement | `319257c6e` |
| K3 | done | Narrow kernel rationale/comments around `PoC` maintenance so they no longer claim PTW universally reads from `PoC` | Kernel comments describe `PoC` as a conservative translation-metadata maintenance choice | `18e8d2366` |
| V1 | done | Build/verification pass for the changed kernel paths | Clean Orin `sel4test` build succeeds after the three kernel commits | n/a |
| V2 | done | Runtime validation against Orin `sel4test` using the rebuilt image | Old SCC/ACI RAS family still reproduces at the same detector PCs, so the implemented lifecycle fixes are partial only | n/a |
| K4 | done | Harden live translation-slot retirement/publication for leaf unmap and parent PT publication | `unmapPage()`, `unmapPageTable()`, and `performPageTableInvocationMap()` use explicit helper-based live slot publication/retirement instead of open-coded raw stores | `64c32ceff` |
| K5 | done | Split valid-to-valid leaf replacement away from first publication in `performPageInvocationMap()` | Live remap path uses an explicit retire/TLBI/publish sequence, while first publication keeps the simpler publish path | `57d3eb528` |
| K6 | done | Review and, if required, strengthen EL2 single-address TLBI helper sequencing | `invalidateLocalTLB_VMID()` / `invalidateLocalTLB_IPA_VMID()` now complete the invalidate phase explicitly before restoring `VTTBR_EL2` | `c803b03c9` |
| K7 | done | Experimental full-ASID retirement for the dominant leaf-unmap path | `unmapPage()` retires the live leaf slot with full-ASID invalidation instead of VA-targeted invalidation | `7523f5b2b` |
| D1 | done | Add PT-liveness side metadata scaffolding keyed by PT base pointer | PT-liveness table and helper API exist; PT map/unmap/finalise now exercise non-invasive tracking/assertion hooks; clean Orin build passes | `e99309d5b` |
| D2 | done | Replace frame-cap mapped address storage with direct leaf-slot identity and reconstruct `vaddr` from PT metadata on transitional slow paths | Frame caps now store tagged direct slot identity for runtime mappings; remap checks compare slot identity; exact `vaddr` is reconstructed from PT metadata where still needed; clean Orin build passes | `336551859` |
| D3 | done | Wire runtime frame map/unmap/finalise to PT-liveness and retire tagged frame mappings by stored slot identity | First publication now increments PT live-leaf count; runtime unmap/finalise first try direct slot-identity retirement instead of rediscovering the leaf through a live walk; clean Orin build passes | `e6c76c95e` |
| D3a | done | Let PT liveness drain after PT teardown instead of asserting zero before PT finalise | Tagged frame-cap teardown can consume outstanding PT live-leaf pins after parent PT teardown; PT finalise sanitises first and only forgets tracking once the count drains to zero; clean Orin build passes | `d29ae0338` |
| S1 | done | Source study for a stronger end-to-end live descriptor retirement/observation protocol | Concrete protocol checklist exists for publication, retirement, ownership, and reuse; current verdict is that publication and some retirement tightening fit the current design, but ownership/reuse are where the current metadata model likely stops short | n/a |
| V6 | done | Runtime validation after `D2` | Failure persists, but the runtime shape shifts again into a pure ACI flood with `ACE AW Decode Error` and a new high-address family; hot ELR values remained in relocated high form and did not resolve directly with `addr2line` | n/a |
| V7 | done | Runtime validation after `D3` | Extended wait reaches seL4 and reproduces the older SCC+ACI low-address family at `0x80800234b4`, then hits a new D3 assertion because page-table finalisation sees nonzero PT live-leaf count | n/a |
| V8 | done | Runtime validation after `D3a` | The PT-liveness assertion is gone, but the image now regresses earlier: both reruns fault immediately after `Enabling hypervisor MMU and jumping to entry point...`; the reported exception PC `0x819A83948` resolves, via the UEFI exception stub and the elfloader image base, to the elfloader's `flush_dcache_range()` `dc civac` instruction | n/a |
| F1 | planned | Follow-up review of valid-to-valid remap ordering in `performPageInvocationMap()` | Source-backed decision on whether the remaining remap subcases need BBM-style treatment | pending |
| V3 | done | Runtime validation after `K4` and `K5` | Failure shape changes materially, but the run still degrades into a persistent ACI-only RAS flood and does not complete cleanly | n/a |
| V4 | done | Runtime validation after `K6` | No meaningful improvement over `V3`; the run still degrades into a persistent ACI-only RAS flood and does not complete cleanly | n/a |
| V5 | done | Runtime validation after `K7` | Failure snaps back toward the older SCC+ACI plus `pte_ptr_get_pte_type()` family, so full-ASID leaf retirement does not rescue the current API | n/a |

## Implemented Kernel Changes

### K1

Commit:

1. `aefb6dbf6` `arm64: sanitise translation objects on creation`

Effect:

1. `VSpace` and `PageTable` creation now goes through one dedicated
   translation-object sanitisation helper
2. helper fills safe-invalid entries and publishes them with the stronger
   maintenance family instead of creation-time `PoU`-only publication

### K2

Commit:

1. `319257c6e` `arm64: sanitise translation objects on final delete`

Effect:

1. final mapped page-table deletion now retires reachability and sanitises the
   PT memory in the same AArch64 arch path
2. final VSpace-root deletion now sanitises the root memory in the same AArch64
   arch path after ASID retirement

### K3

Commit:

1. `18e8d2366` `arm64: narrow PoC page-table maintenance rationale`

Effect:

1. comments now describe `PoC` as a conservative maintenance choice for
   translation metadata on Orin-class systems
2. comments no longer overstate the architecture as universally defining PTW as
   a `PoC` consumer

### K4

Commit:

1. `64c32ceff` `arm64: factor live translation slot updates`

Effect:

1. `unmapPage()`, `unmapPageTable()`, and `performPageTableInvocationMap()` no
   longer open-code translation-slot stores
2. live slot publication/retirement now goes through explicit helper functions
   in `vspace.c`
3. this does not yet change valid-to-valid leaf replacement semantics; that is
   reserved for `K5`

### K5

Commit:

1. `57d3eb528` `arm64: split first publication from leaf replacement`

Effect:

1. `performPageInvocationMap()` now distinguishes first publication from
   valid-to-valid replacement
2. valid-to-valid replacement goes through explicit retire/TLBI/publish helper
   flow instead of a direct overwrite
3. first publication keeps the simpler publish-only path

## Post-K4/K5 Validation

Runtime validation run:

1. request `20260315-194544`
2. profile `sel4test`
3. image `orinagx_sel4test/images/sel4test-driver-image-arm-orinagx`
4. status: canceled after clear reproduction of a changed but still failing RAS
   pattern

Observed differences from the pre-`K4`/`K5` run:

1. the old dominant `SCC` + `ACI` pair was not seen in the sampled output
2. the old hot `ELR_EL3` values at:
   - `pte_ptr_get_pte_type()` lines 2272/2273
   - `unmapPage()` line 1071
   were no longer the dominant signature
3. new repeated `ELR_EL3` values resolve to:
   - `0x8273bf840` -> `lookupPTSlot()` line 680
   - `0x8273bf408` -> `pte_pte_table_ptr_get_pt_base_address()` line 2294
   - `0x8273bf820` -> `pte_get_page_base_address()` line 225
   - `0x8273bedb0` / `0x8273bedc0` / `0x8273bfa00` -> nearby decode / address
     helpers in the same walk family
4. the run degraded into a long `ACI`-only flood instead of the older
   `SCC`/`ACI` detector pattern

Interpretation after `V3`:

1. `K4`/`K5` changed the live-path behavior materially
2. but they did not remove the underlying Orin failure mode
3. the remaining highest-priority suspect is now the live table-walk / decode
   / EL2 single-address TLBI interaction, not the older open-coded overwrite
   shape in `unmapPage()` / `performPageInvocationMap()`

## Current State

Kernel repo status after the three commits:

1. no tracked kernel changes remain uncommitted
2. unrelated untracked files still exist in the kernel repo:
   - `.fff.swp`
   - `tools/ftrace-index/`

Verification state:

1. source-level review complete for the implemented changes
2. clean verification build completed successfully:
   - `make mrproper`
   - `make orinagx_defconfig`
   - `make sel4test`
3. resulting image was generated at:
   - `orinagx_sel4test/images/sel4test-driver-image-arm-orinagx`
4. runtime validation run submitted via Autopilot:
   - request `20260315-191401`
   - profile `sel4test`
   - binary `orinagx_sel4test/images/sel4test-driver-image-arm-orinagx`
5. runtime validation result:
   - run was canceled after clear reproduction of the old RAS family
   - repeated `SCC` / `ACI` floods remained present
   - surviving hot `ELR_EL3` values still resolve to:
     - `0x808001a61c` -> `pte_ptr_get_pte_type()` line 2272
     - `0x808001a620` -> `pte_ptr_get_pte_type()` line 2273
     - `0x808001a728` -> `unmapPage()` line 1071
     - `0x8080022d08` -> `pte_ptr_get_pte_type()` line 2273
6. interpretation:
   - the implemented creation/final-delete sanitisation fixes did not remove the
     dominant live detector path
   - remaining investigation priority should stay on untouched live paths around
     leaf unmap / lookup / slot decoding rather than on already-fixed
     translation-object creation and final-cap teardown
7. narrowed live-path reading after `V2`:
   - `unmapPage()` still interrupts at line 1071, immediately before
     `invalidateTLBByASIDVA(asid, vptr)`
   - the dominant hot PCs remain `pte_ptr_get_pte_type()` line 2272/2273, which
     means the failing reader is still the live walk / decode path, not the new
     creation/final-delete sanitisation helper
   - untouched high-priority candidates are now:
     - live leaf unmap plus single-address TLBI under EL2
     - live parent page-table publication in `performPageTableInvocationMap()`
     - live leaf remap/publication in `performPageInvocationMap()`
8. concrete next-fix proposal after `V2`:
   - introduce one dedicated helper for single-slot translation retirement:
     - store safe-invalid entry
     - conservative translation-metadata maintenance
     - required TLBI
   - introduce one dedicated helper for first publication of parent/leaf slots:
     - publish entry
     - conservative translation-metadata maintenance
   - split valid-to-valid leaf replacement into a separate path:
     - retire old entry to safe invalid
     - conservative maintenance
     - TLBI/wait
     - publish new entry
     - conservative maintenance
   - review whether the EL2 single-address TLBI helper needs stronger local
     sequencing once the live paths are split cleanly
9. after `V3`, the priority narrows further:
   - `K4` and `K5` changed the signature, so they were not no-ops
   - the next kernel step should focus on `K6`, i.e. the EL2
     single-address-TLBI-side sequencing and its interaction with live walk /
     decode

### K6

Commit:

1. `c803b03c9` `arm64: complete TLBI before VTTBR restore`

Effect:

1. `invalidateLocalTLB_VMID()` and `invalidateLocalTLB_IPA_VMID()` now make
   the end of the invalidate phase explicit before restoring the caller's
   `VTTBR_EL2`
2. this keeps the temporary VMID context active until the helper-local TLBI
   sequence is fully completed
3. the change is intentionally narrow; it does not introduce a broader new EL2
   barrier model outside the temporary-VMID TLBI helper

## Post-K6 Validation

Runtime validation run:

1. request `20260315-195906`
2. profile `sel4test`
3. image `orinagx_sel4test/images/sel4test-driver-image-arm-orinagx`
4. status: canceled after clear reproduction of the post-`K4`/`K5` failure
   shape

Observed result:

1. the run progressed into the same broad failing regime seen after `K4`/`K5`
2. sampled output again showed a persistent `ACI`-only flood
3. sampled `ELR_EL3` values again clustered in the same walk/decode family,
   though the raw reported addresses were not directly resolvable with
   `addr2line` due to the high relocated address form in the log

Interpretation after `V4`:

1. `K6` did not produce a meaningful improvement over `V3`
2. the remaining issue is unlikely to be fixed by a narrow “complete TLBI
   before `VTTBR_EL2` restore” adjustment alone
3. further work should move away from barrier-only tweaks and toward a deeper
   investigation of why Orin keeps reporting ACI-side faults during the
   surviving walk/decode activity

### K7

Commit:

1. `7523f5b2b` `arm64: experiment with full-ASID leaf retirement`

Effect:

1. `unmapPage()` no longer retires the dominant leaf-unmap slot with
   VA-targeted invalidation
2. it now publishes safe-invalid and invalidates the whole ASID instead
3. this is explicitly an isolation experiment for the surviving teardown-side
   reader, not a claimed final fix

## Post-K7 Validation

Runtime validation run:

1. request `20260315-204025`
2. profile `sel4test`
3. image `orinagx_sel4test/images/sel4test-driver-image-arm-orinagx`
4. status: canceled after the failure shape was clear

Observed result:

1. the experiment did not remove the failure
2. instead of preserving the post-`K4`/`K6` walk/decode-heavy `ACI`-only
   pattern, the run snapped back toward the older mixed `SCC` + `ACI` family
3. repeated `ELR_EL3` values again resolved to:
   - `0x808001a62c` -> `pte_ptr_get_pte_type()` line 2272
   - `0x808001a630` -> `pte_ptr_get_pte_type()` line 2273
4. repeated reported bad addresses clustered again near the old `0x7ffffxxx`
   family, including:
   - `0x800000007ffffe80`
   - `0x800000007fffff00`

Interpretation after `V5`:

1. full-ASID retirement for the dominant leaf-unmap path is not sufficient to
   rescue the current API
2. the result is still useful because it shows the surviving issue is sensitive
   to the retirement protocol shape
3. but it weakens the idea that one stronger invalidate choice on the current
   frame-cap / live-walk design will be enough by itself
4. further work should focus on either:
   - a materially stronger end-to-end live descriptor retirement/observation
     protocol, or
   - a design that reduces `unmapPage()` dependence on re-trusting a live slot

## Current Study Direction

After `K7`, the near-term recommended path is no longer another invalidate-flavor
experiment. It is the source study now tracked as `S1`:

1. define the minimum protocol strong enough that a later serialized
   `lookupPTSlot()` / `unmapPage()` reader cannot still trust a stale table or
   leaf descriptor
2. compare current seL4 against that protocol in four areas:
   - publication
   - retirement
   - ownership across invalidation
   - reuse gating
3. decide whether current seL4 can plausibly be lifted to that stronger
   protocol without changing cap metadata
4. if not, promote the mapping-metadata redesign direction as the next major
   path

Result of `S1`:

1. publication can probably be strengthened within the current design
2. some retirement-side tightening can probably also be done within the current
   design
3. ownership across invalidation is the weakest area:
   - frame caps do not preserve direct slot identity
   - slots do not carry an invalid-but-owned state
4. generic reset/retype also weakens reuse gating because untyped reset does
   not naturally encode “retirement completed” for translation metadata
5. current best direction:
   - stop treating metadata redesign as a distant fallback
   - treat it as the likely next major path if any further protocol-only
     tightening still fails

Follow-on redesign study result:

1. the smallest cap-local redesign that looks technically plausible is to
   replace exact stored mapped `vaddr` with direct leaf-slot identity
2. on the active 40-bit EL2 user-VA configuration, the existing 48-bit
   `capFMappedAddress` field can plausibly encode:
   - parent PT base page number
   - leaf slot index
3. but that is not sufficient by itself because it does not solve parent PT
   lifetime/reuse
4. current redesign ranking:
   - strongest complete candidate: direct leaf-slot identity plus explicit
     parent-PT liveness
   - smaller but incomplete candidate: direct leaf-slot identity alone
   - larger alternative: reverse-map side metadata

Minimal parent-PT liveness sketch:

1. current `capPTIsMapped` only expresses tree-link state, not dependency
   liveness for mapped frame caps
2. smallest plausible complete design is:
   - frame caps store direct leaf-slot identity
   - side metadata keyed by PT base pointer tracks live-leaf pin/count
   - PT free/reuse is blocked until the live-leaf count reaches zero
3. this now looks like the smallest redesign candidate that is complete enough
   to avoid recreating the same trust problem one level lower

Implementable redesign split:

1. `D1`: add side metadata keyed by PT base pointer for live-leaf liveness
2. `D2`: replace frame-cap stored mapped `vaddr` with direct leaf-slot identity
3. `D3`: wire frame map/unmap/finalise to increment/decrement PT liveness and
   retire by direct slot identity
4. `D4`: gate PT finalise and untyped PT reuse on zero live-leaf count
5. `D5`: adapt secondary fallout paths such as page flush and capDL/debug

Primary touch points:

1. `kernel/include/arch/arm/arch/64/mode/object/structures.bf`
2. `kernel/include/arch/arm/arch/64/mode/model/statedata.h`
3. `kernel/src/arch/arm/64/model/statedata.c`
4. `kernel/src/arch/arm/64/kernel/vspace.c`
5. `kernel/src/arch/arm/64/object/objecttype.c`
6. `kernel/src/object/untyped.c`

Recommended order:

1. metadata first
2. cap identity next
3. dominant `unmapPage()` path switch after both exist
4. hard PT teardown/reuse gating after the reader side is converted

## Next Actions

1. Implement `K4` first:
   - replace open-coded raw slot retirement/publication in:
     - `unmapPage()`
     - `unmapPageTable()`
     - `performPageTableInvocationMap()`
2. Implement `K5` next:
   - split `performPageInvocationMap()` into first-publication vs
     valid-to-valid replacement handling
3. Implement `K6` only after `K4`/`K5` land:
   - reassess the surviving `unmapPage()` line-1071 interrupt point against the
     cleaned-up live slot helpers
4. Keep remap-ordering review as a separate follow-up item, but it now feeds
   directly into `K5`.
