# Plan: Eliminating Remaining RAS Errors in seL4 on Orin AGX

Date: 2026-03-17
Status: Proposed
Scope: seL4 kernel AArch64 EL2 page table lifecycle during bulk cap revocation

## Problem Summary

During `cteRevoke` (triggered by `CANCEL_BADGED_SENDS_0002`), the kernel deletes hundreds of frame and PT caps in a tight loop. Each deletion does `publishTranslationSlot(invalid)` → `TLBI` → `DSB` → `ISB`. Despite correct per-entry barriers, the Cortex-A78AE speculative page table walker occasionally follows a stale table entry to freed/sanitized PT memory, reads an address below DRAM (`0x7FFFxxxx`), and triggers an SCC/ACI RAS error. Experiments E1–E6 confirmed that no single-entry barrier strengthening fixes the problem.

The fundamental issue: **PT memory is sanitized/reused while a speculative walker on another core (or a speculative prefetch on the same core) may still hold a cached reference to the parent table entry pointing to that PT.** Linux avoids this through deferred freeing.

## Solution 1: Two-Phase Cap Revocation (Recommended)

**Concept**: Split the `cteRevoke` loop into two phases when architectural caps are involved. Phase 1 invalidates all PTEs and parent entries. Phase 2 (after a global drain barrier) sanitizes and frees PT memory.

### Implementation

Modify `Arch_finaliseCap` for `cap_page_table_cap` to **defer sanitization**:

```
Phase 1 (during cteRevoke loop, per-cap):
  - Frame caps: unmapPageBySlotID → retire PTE + TLBI (as today)
  - PT caps: drainPTLiveLeaves + unmapPageTable + invalidate parent
    BUT skip sanitiseTranslationObject — just mark PT as "pending sanitise"

Phase 2 (after cteRevoke loop completes, or at preemptionPoint):
  - Single invalidateTranslationAll() + DSB(sy) + ISB
  - Then sanitise all pending PT objects
  - Then ptRefcountForget all
```

### Where to hook Phase 2

Option A: At `preemptionPoint()` in `cteRevoke` (line 543 of cnode.c). Already called after every cap deletion. Add a "drain pending PT sanitise" call here.

Option B: In `finaliseSlot` — accumulate a small static list of PT pointers to sanitise, flush when full or when revocation completes.

### Code changes

1. **`objecttype.c`** (`cap_page_table_cap` case): Remove `sanitiseTranslationObject` and `invalidateTranslationAll`+`dsb`+`isb` from inline finalize. Instead, append PT pointer to a per-CPU pending list.

2. **New function `drainPendingPTSanitise()`**: Issues `invalidateTranslationAll()` + `dsb(sy)` + `isb()`, then walks the pending list calling `ptRefcountForget` + `sanitiseTranslationObject` for each.

3. **`cteRevoke`**: Call `drainPendingPTSanitise()` at the preemption point and after the loop exits.

4. **Static pending list**: Fixed-size array (e.g., 16 entries) in kernel BSS. If full, flush immediately (with TLBI+DSB+ISB) before continuing.

### Pros

- **Directly addresses root cause**: PT memory is never accessed by the walker after the global drain barrier
- **Matches Linux's proven approach** (mmu_gather / batched TLB + deferred free)
- **Minimal API change**: No change to seL4 syscall semantics or cap model
- **Amortizes TLBI cost**: One global TLBI per batch instead of one per PT — actually faster for bulk teardown
- **Deterministic**: No timing dependency, no probabilistic improvement

### Cons

- **Kernel state**: Requires a per-CPU pending list (static, bounded) — adds ~128 bytes BSS per core
- **Verification impact**: seL4's formal verification would need to model the deferred sanitisation invariant (PT is "logically dead" but memory is still allocated)
- **Complexity in preemption**: If the kernel is preempted between phase 1 and phase 2, the pending list must be drained on context switch — but `preemptionPoint` already handles this
- **Single-core assumption**: The pending list is per-CPU; if caps on different cores reference the same PT, the drain must be cross-core visible (already is, since `invalidateTranslationAll` uses IS variants)

### Risk: Low

The cap system already handles "zombie" caps where finalization is split across multiple steps. This extends that pattern to PT sanitization.

## Solution 2: Safe PT Memory Content After Free

**Concept**: Instead of sanitizing PT memory with `pte_pte_invalid_new()` (which contains non-zero address bits), fill freed PT memory with PTEs whose output address points to a **dedicated safe physical page within DRAM**. The walker can follow them without RAS — it just reads harmless data.

### Implementation

1. Reserve one 4KB physical page at boot (from the kernel's reserved region, above DRAM base `0x80000000`).
2. Define `pte_pte_safe_invalid_new()` that writes `safe_page_paddr | 0x0` (bits[1:0]=0, so architecturally invalid, but address bits point to valid DRAM).
3. Use this in `sanitiseTranslationObject` and `pte_pte_invalid_new()`.

### Why E6 isn't the same thing

E6 changed `pte_pte_invalid_new()` to write zero, but zero means PA=0 which is *below* DRAM on Orin. A safe-invalid PTE needs PA >= `0x80000000`.

### Pros

- **Very simple change**: ~10 lines of code
- **No architectural restructuring**: No deferred lists, no two-phase revocation
- **Addresses the symptom directly**: Even if the walker reads stale/sanitized PT content, the address bits point to valid DRAM → no RAS error
- **Easy to verify**: Isolated change with clear invariant

### Cons

- **Doesn't fix the root cause**: The walker is still following stale entries into freed memory — it just doesn't trigger an error anymore. This masks a real architectural violation.
- **ARM says bits[1:0]=0 means "don't follow"**: If the walker is truly ignoring the invalid bit and following the address anyway, that's a hardware erratum, not a software problem. This solution would paper over it.
- **Doesn't help if PT memory is retyped to non-PT objects**: After retype, the memory contains arbitrary data — no safe PTE content. However, retype already calls `clearMemory` which writes zeros, so this is the same as E6 (PA=0).
- **May not work**: If the walker is reading the old (pre-sanitise) PT content from cache, the safe-invalid content hasn't been written yet. The race is about timing, not content.

### Risk: Medium

This might reduce but not eliminate errors, similar to E6. The walker race is about reading old data, not new data.

## Solution 3: ASID Invalidation Before PT Reuse

**Concept**: When a VSpace is torn down, immediately invalidate its ASID in hardware (not just in the ASID map). This prevents any walker from using cached translations for that ASID, regardless of what PT memory contains.

### Implementation

1. In `deleteASID()`: After removing from ASID pool, issue `TLBI ASIDE1IS, <asid>` + `DSB(sy)` + `ISB`.
2. In `Arch_finaliseCap` for `cap_vspace_cap`: Ensure ASID deletion happens **before** any PT finalization.
3. Key insight: `cteRevoke` processes children in MDB order. If the VSpace cap is the parent, it's finalized **last** (after all PT and frame caps). But `deleteASID` should be callable early.

### Problem with MDB ordering

`cteRevoke` walks children of a cap. The VSpace cap itself is the parent — it's NOT deleted by `cteRevoke`, only its children are. The children include PT caps and frame caps. The VSpace cap is deleted separately (if at all). So ASID invalidation at VSpace finalization doesn't help — it happens too late or not at all during the revocation that triggers RAS.

### Alternative: Invalidate ASID at start of bulk revocation

Add a pre-revocation hook: when `cteRevoke` detects that children include VSpace-related caps, invalidate the ASID first.

### Pros

- **Architecturally clean**: ASID invalidation is the ARM-recommended way to discard old translations
- **No deferred state**: No pending lists or quarantine

### Cons

- **Doesn't prevent walker from using cached table entries**: ASID invalidation removes TLB entries, but the walker may have already started a walk using the old TLB entry before the TLBI. The walk continues using physical addresses, not ASID — ASID only gates TLB lookup, not in-flight walks.
- **MDB ordering problem**: The ASID isn't known from the children being revoked; it requires traversing cap relationships upward.
- **Already tested (partially)**: E2 added full TLBI at PT finalize — no effect. E3 added VMALLS12E1IS — no effect. ASID-scoped TLBI is weaker than what was already tried.
- **seL4 ASID model complexity**: ASIDs are managed through pools and mapped to HW ASIDs; early invalidation would break the ASID lifecycle invariants.

### Risk: High

Experiments E2/E3 already showed that TLBI-based approaches don't close the race window. This is unlikely to work.

## Solution 4: Quarantine Pool with Grace Period

**Concept**: When PT memory is freed, place it in a per-CPU quarantine ring buffer instead of immediate reuse. Drain the quarantine only after a "grace period" — a subsequent kernel entry where DSB+ISB is guaranteed.

### Implementation

1. **Quarantine buffer**: Per-CPU ring buffer of `(pte_t *pt, word_t sizeBits)` tuples, size 32–64 entries.
2. **On PT finalize**: Instead of `sanitiseTranslationObject`, enqueue PT pointer.
3. **On kernel entry** (syscall/IRQ/fault handler top): `drainQuarantine()` — DSB+ISB already executed by exception entry, so all walks from previous user context are complete. Sanitise all queued PTs.
4. **Overflow**: If quarantine is full, force-drain with `invalidateTranslationAll` + DSB + ISB before enqueuing.

### Pros

- **Strongest temporal guarantee**: By deferring to next kernel entry, any walk that was in-flight during the previous user execution is guaranteed complete (exception entry implies DSB+ISB on ARM).
- **No changes to cap revocation structure**: `cteRevoke` stays exactly as-is.
- **Matches Linux RCU philosophy**: Memory isn't freed until a quiescent point.

### Cons

- **Kernel BSS overhead**: ~1KB per core for the ring buffer.
- **Delayed memory availability**: PT memory isn't reusable until next kernel entry. For back-to-back revoke→retype in the same syscall, this could delay availability.
- **Verification complexity**: The quarantine introduces a new invariant — PT memory is "logically free but physically occupied." Formal verification must model this.
- **Drain on syscall entry adds latency**: Even if the quarantine is empty, checking it costs a few cycles per kernel entry. Could be gated by a flag.
- **Doesn't help if retype happens in same syscall**: If the test does revoke→retype→map in one invocation, the quarantine won't drain until the next entry.

### Risk: Low–Medium

Sound in principle, but the "same syscall retype" edge case needs analysis. If sel4test does revoke and retype in separate syscalls (likely), this works perfectly.

## Solution 5: Break-Before-Make with Intermediate Zero State

**Concept**: Before writing `pte_pte_invalid_new()` to a PTE, first write a **zero PTE** (all 64 bits zero), DSB, then write the final invalid PTE. This ensures that any walker seeing a mid-transition PTE reads all-zeros (PA=0, invalid). Combined with ensuring PA=0 doesn't trigger RAS (e.g., by mapping PA=0 to a safe DRAM alias in the interconnect config).

### Pros

- Follows ARM BBM (break-before-make) to the letter
- Minimal code change

### Cons

- **PA=0 still triggers RAS on Orin**: The interconnect reports PA=0 as illegal. E6 already proved this — zeroed PTEs still caused RAS.
- **Can't remap PA=0 in interconnect**: That's firmware/SOC config, not software.
- **Doesn't address the freed PT memory problem**: The walker follows the parent entry to PT memory, not the leaf PTE itself.

### Risk: High

E6 disproved this approach.

## Recommendation

| Criterion | Sol 1 | Sol 2 | Sol 3 | Sol 4 | Sol 5 |
|-----------|-------|-------|-------|-------|-------|
| Addresses root cause | Yes | No (masks) | Unlikely | Yes | No |
| Proven in other systems | Linux mmu_gather | — | — | Linux RCU | — |
| Implementation complexity | Medium | Low | High | Medium | Low |
| Verification impact | Medium | Low | High | Medium | Low |
| Confidence of success | High | Low | Low | High | None |
| Performance impact | Positive (amortized TLBI) | None | Negative | Small negative | None |

**Solution 1 (Two-Phase Revocation)** is the clear winner. **Solution 4 (Quarantine)** is the backup if Solution 1 proves too invasive to the cap finalization flow.

### Implementation order

1. **Implement Solution 1** — two-phase revocation with deferred PT sanitization
2. **Validate** — run sel4test 10+ times, expect zero RAS
3. **If still fails** — add Solution 4 (quarantine to next kernel entry) as a stronger temporal guarantee
4. **Optionally** — add Solution 2 (safe PA in invalid PTEs) as defense-in-depth

## Key Files

- `kernel/src/object/cnode.c` — `cteRevoke`, `cteDelete`, `emptySlot`
- `kernel/src/arch/arm/64/object/objecttype.c` — `Arch_finaliseCap` (PT/frame/vspace cases), `sanitiseTranslationObject`
- `kernel/src/arch/arm/64/kernel/vspace.c` — `publishTranslationSlot`, `retireTranslationSlot*`, `drainPTLiveLeaves`, `unmapPageBySlotID`, `unmapPageTable`, PT refcount API
- `kernel/include/arch/arm/arch/64/mode/kernel/vspace.h` — slot identity encoding/decoding
- `kernel/include/arch/arm/arch/64/mode/model/statedata.h` — PT refcount table definition

## References

- Prior experiments: `docs/plans/orin-kernel-runtime-ras-analysis-2026-03-17.md`
- ARM speculative PTW research: `docs/platforms/orin-agx/reference/arm-speculative-ptw-research.md`
- Frame cap redesign (current branch): `docs/plans/orin-frame-cap-slot-identity-redesign-plan-2026-03-15.md`
- Linux KVM speculative PTW fix (Marc Zyngier, April 2023): https://lwn.net/Articles/929006/
- ARMv7 page table free race: https://linux-arm-kernel.infradead.narkive.com/JFAnhCcS/rfc-patch-2-2-armv7-invalidate-the-tlb-before-freeing-page-tables
