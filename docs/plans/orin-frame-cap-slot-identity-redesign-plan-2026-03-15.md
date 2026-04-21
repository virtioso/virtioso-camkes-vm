# Frame Cap Slot Identity Redesign Plan

Date: 2026-03-15
Updated: 2026-03-16
Status: Implemented and validated — zero RAS errors end-to-end on Orin AGX sel4test
Scope: ARM64 only (initial). Other architectures unaffected.
Supersedes: D-series experimental patches (D1/D2/D3) in current kernel tree.

## Kernel Branch Context

Upstream base commit: `717cf90201f5457751892a935a0c985a853aa94c` (seL4 master).

All commits on top of that are local work, falling into two categories:

### Orin AGX platform support

```
be93e2765  arm: Add Cortex-A78 CPU support
07b8231dc  arm64: Add SDEI (Software Delegated Exception Interface) support
7f648bc02  arm: Add NVIDIA Orin AGX (Tegra234) platform support
e16a0a667  orinagx: Add SDEI RAS error handling
ddd498725  orinagx: Start RAM at 0x80032000 to avoid RAS errors
10353145e  orinagx: Use RAM start at 0x80000000 with 768MB size
b537280aa  orinagx: Add BPMP/HSP device nodes for VM guest support
```

### FOR-UPSTREAM fixes (independent correctness improvements)

```
1718a85f7  kernel/boot: Add option for verbose debug
136478271  FOR-UPSTREAM: arm64: Override pte_pte_invalid_new() with safe PA for speculative PTW
efdce541a  FOR-UPSTREAM: arm64: Initialize armKSGlobalUserVSpace with safe invalid PTEs
1bf44e557  FOR-UPSTREAM: arm64: Initialize VSpace/PageTable objects with safe invalid PTEs
8a282fba1  FOR-UPSTREAM: arm64: Fix AARCH64_VSPACE_S2_START_L1 config
0d5cee148  FOR-UPSTREAM: arm64: Use valid PT base during VMID switch for TLB invalidation
f6aa6669d  FOR-UPSTREAM: arm64: Split cache.c into ARM32 and ARM64 versions
```

### RAS error investigation and fix attempts

```
eba5a3b6e  arm: Fix page table cache coherency - use dc civac (PoC) not dc cvau (PoU)
aefb6dbf6  arm64: sanitise translation objects on creation
319257c6e  arm64: sanitise translation objects on final delete
18e8d2366  arm64: narrow PoC page-table maintenance rationale
64c32ceff  arm64: factor live translation slot updates
57d3eb528  arm64: split first publication from leaf replacement
c803b03c9  arm64: complete TLBI before VTTBR restore
7523f5b2b  arm64: experiment with full-ASID leaf retirement
e99309d5b  arm64: add pt liveness metadata scaffolding
336551859  arm64: store frame mapping slot identity
e6c76c95e  arm64: retire mapped frames by slot identity
03f5a0a92  arm64: experiment with pt liveness drain on pt teardown
```

The RAS fix attempts progressed from simple cache maintenance fixes (`eba5a3b6e`)
through increasingly structural changes, culminating in the D-series cap
identity and PT liveness work. None fully eliminated the Orin RAS errors, which
motivated the analysis that the problem is structural (see
[orin-unmapPage-design-deficiency-analysis-2026-03-15.md](/home/hlyytine/tii-sel4/docs/plans/orin-unmapPage-design-deficiency-analysis-2026-03-15.md))
and requires a proper cap-format redesign rather than incremental fixes.

## Goal

Replace the experimental D-series kernel-side bookkeeping (hash table + tag-bit
dual encoding) with a proper cap-format change that stores direct leaf-slot
identity in frame caps. This eliminates the mandatory live tree re-walk in
`unmapPage()` and removes the structural design deficiency documented in
[orin-unmapPage-design-deficiency-analysis-2026-03-15.md](/home/hlyytine/tii-sel4/docs/plans/orin-unmapPage-design-deficiency-analysis-2026-03-15.md).

## What The D-Series Got Right (And What It Got Wrong)

The D-series proved the approach: storing slot identity in frame caps and using
PT liveness tracking allows direct retirement without re-walking the live tree.
The runtime experiments confirmed the structural direction is correct.

What it got wrong is the implementation strategy. By treating the cap format as
immutable, it was forced into:

1. A 512 KB BSS hash table (`armKSPTLivenessTable[16384]`) with open addressing,
   tombstones, and linear probing
2. A tag-bit dual encoding of `capFMappedAddress` (bit 47 discriminates old
   vaddr from new slot identity)
3. Redundant vaddr reconstruction through hash table lookups
   (`armFrameCapGetMappedVaddr()` must find the PT's liveness entry to get
   `mapped_vaddr_base`, then combine with slot index)
4. Legacy fallback paths for boot-created and untagged caps

All of this disappears with a proper cap-format change.

## Design

### New Frame Cap Layout

Current layout
([structures.bf:23](/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/64/mode/object/structures.bf#L23)):

```
block frame_cap {
    field capFMappedASID             16
    field_high capFBasePtr           48

    field capType                    5
    field capFSize                   2
    field_high capFMappedAddress     48    -- exact virtual address
    field capFVMRights               2
    field capFIsDevice               1
    padding                          6
}
```

New layout:

```
block frame_cap {
    field capFMappedASID             16
    field_high capFBasePtr           48

    field capType                    5
    field capFSize                   2
    field_high capFMappedPTBase      48    -- parent PT object physical base
    field capFSlotIndex              9     -- slot index within parent PT
    field capFVMRights               2
    field capFIsDevice               1
    padding                          -2    -- DOES NOT FIT, see analysis below
}
```

**Bit budget problem.** The second word has 64 bits. Fixed fields consume:
`capType(5) + capFSize(2) + capFVMRights(2) + capFIsDevice(1) = 10 bits`.
That leaves 54 bits. We need `capFMappedPTBase + capFSlotIndex`. On AArch64
with 48-bit PA and 4K-aligned PTs: PT base needs `48 - 12 = 36` bits, slot
index needs 9 bits. Total: 45 bits. That fits in 54 bits with 9 bits to spare.

But `field_high` in seL4's bitfield generator means the field occupies the
high-order bits of the word and is shifted on access, implying the stored value
is a physical address with low bits implicitly zero. We need to use the 48-bit
`field_high` slot for the PT base (which is page-aligned, so the low 12 bits
are always zero -- `field_high 48` stores 36 meaningful bits).

Revised layout that fits:

```
block frame_cap {
    field capFMappedASID             16
    field_high capFBasePtr           48

    field capType                    5
    field capFSize                   2
    field_high capFMappedPTBase      48    -- parent PT phys base (4K-aligned, 36 meaningful bits)
    field capFSlotIndex              3     -- PROBLEM: only 3 bits left
    field capFVMRights               2
    field capFIsDevice               1
    padding                          3
}
```

That only gives 3 bits for slot index (8 slots). AArch64 PTs have 512 entries
(9-bit index). This does not fit.

### Resolution: Pack Slot Identity Into The 48-Bit Field

The solution is to pack both PT base and slot index into a single 48-bit
`field_high` field, exactly as the D-series does -- but as the **canonical cap
format**, not a tag-bit hack.

```
block frame_cap {
    field capFMappedASID             16
    field_high capFBasePtr           48

    field capType                    5
    field capFSize                   2
    field_high capFMappedSlotID      48    -- packed: PT base page number + slot index
    field capFVMRights               2
    field capFIsDevice               1
    padding                          6
}
```

Encoding of `capFMappedSlotID` (48 bits available after `field_high` shift):

```
bits [47:9]  = PT base physical address >> 12  (39 bits, covers 51-bit PA space)
bits [8:0]   = slot index within PT            (9 bits, covers 512 entries)
```

On the current Orin 40-bit PA config, only 28 of the 39 PT-base bits are
needed. On a hypothetical 48-bit PA config, all 39 bits are used (48 - 12 + 3
bits of headroom from the 9-bit split). This covers all practical AArch64
configurations.

Unmapped state: `capFMappedASID == asidInvalid` means unmapped. When unmapped,
`capFMappedSlotID` is zero. No tag bit needed.

### Vaddr Reconstruction

The new cap format does not store the mapped virtual address. Callers that need
it must reconstruct it. Analysis of all consumers:

| Consumer | Frequency | How to get vaddr |
|----------|-----------|-----------------|
| `unmapPage()` | hot path | **not needed** -- direct slot retirement |
| `performPageInvocationMap()` | hot path | already has vaddr from syscall args |
| `performPageInvocationUnmap()` | hot path | **not needed** -- via direct retirement |
| `Arch_finaliseCap()` (frame) | medium | **not needed** -- via direct retirement |
| `decodeARMFrameInvocation()` remap check | medium | compare slot identity, not vaddr |
| Page flush (EL2 path) | infrequent | reconstruct from PT cap's `capPTMappedAddress` |
| Page flush (EL1 path) | infrequent | reconstruct (same) |
| capDL debug dump | rare/debug | reconstruct (same) |

**Key insight:** The hot paths (unmap, finalize) no longer need vaddr at all.
The map path already has vaddr from the syscall. Only flush and debug need
reconstruction, and both are infrequent.

**Reconstruction method** for the infrequent paths:

```c
vptr_t reconstructVaddrFromSlotID(cap_t cap)
{
    pte_t *ptSlot = slotIDToPtr(cap_frame_cap_get_capFMappedSlotID(cap));
    pte_t *ptBase = ROUND_DOWN(ptSlot, seL4_PageTableBits);
    word_t slotIndex = ptSlot - ptBase;
    vm_page_size_t size = cap_frame_cap_get_capFSize(cap);

    /* Walk from vspace root to find this PT's vaddr contribution */
    asid_t asid = cap_frame_cap_get_capFMappedASID(cap);
    findVSpaceForASID_ret_t find_ret = findVSpaceForASID(asid);
    if (find_ret.status != EXCEPTION_NONE) return 0;

    /* Walk the tree to find which vaddr leads to ptBase */
    return walkTreeForPTBase(find_ret.vspace_root, ptBase, slotIndex, size);
}
```

This is a tree walk -- the same cost as the old `lookupPTSlot()`. But it only
runs on flush/debug paths, not on the hot unmap path. That is the entire point
of the redesign.

**Alternative (simpler, recommended for initial implementation):** Pass vaddr
explicitly through the few paths that need it, avoiding reconstruction
entirely:

1. `performPageInvocationMap()`: already receives vaddr transitively from
   `decodeARMFrameInvocation()` -- pass it as a parameter
2. Page flush: the flush decode path already does `findVSpaceForASID()` + has
   vaddr context. Under EL2, flush uses physical addresses directly
   (`pptr_to_paddr(capFBasePtr) + offset`), so vaddr is only needed for EL1
   stale-mapping validation (which already does its own `lookupPTSlot()`)
3. capDL: debug-only, can reconstruct via tree walk

This means **zero vaddr reconstruction cost** in the production EL2 path.

### PT Liveness Tracking

Even with direct slot identity in the cap, we still need to prevent premature
PT reuse. If a PT object is freed/reused while frame caps still store slot
pointers into it, those pointers become dangling.

**Minimum requirement:** a per-PT counter of live leaf mappings that reference
slots within it.

**Where to store it:**

The D-series uses a 512 KB hash table. We can do much better.

**Option A: Minimal hash table (recommended for initial implementation)**

Same hash table concept but radically smaller:

```c
typedef struct {
    pte_t *pt;          /* key */
    uint16_t leaf_count; /* value */
} pt_refcount_entry_t;
```

8 bytes per entry (down from 32). Table size can be 4096 entries (32 KB) or
even 2048 (16 KB). The number of active PTs in a typical sel4test run is in
the low hundreds.

No `mapped_asid`, no `mapped_vaddr_base`, no tombstones needed (use
robin-hood or cuckoo hashing for simpler deletion).

**Option B: Embed counter in PT object metadata**

Reserve 8 bytes of kernel-managed metadata per PT object. This could be stored
in a secondary allocation alongside the PT, or in a per-PT-pool array. More
complex to implement but eliminates hash table entirely.

**Option C: Counter in page_table_cap**

The `page_table_cap` has 30 bits of padding. We could add a
`capPTLiveLeafCount` field (10 bits, max 1023). Problem: updating the counter
requires finding the PT cap's CTE from the PT base pointer, which is expensive
(CNode walk). Not practical for the hot map/unmap path.

**Recommendation:** Start with Option A. It is simple, proven (D-series
demonstrated the concept), and eliminates 90% of the D-series complexity. The
table is 32x smaller and the entries are trivial.

### Lifecycle Rules

```
Frame Map:
  1. Derive slot identity: ptBase = ROUND_DOWN(ptSlot), index = ptSlot - ptBase
  2. Encode into capFMappedSlotID
  3. Set capFMappedASID
  4. Increment PT refcount for ptBase
  5. Publish PTE with one-copy store + walker visibility

Frame Unmap / Finalize:
  1. Decode capFMappedSlotID → ptSlot
  2. Validate: *ptSlot is page-type and base matches capFBasePtr
  3. Retire: write invalid PTE, clean cache, TLBI
  4. Decrement PT refcount for ptBase
  5. Clear capFMappedSlotID and capFMappedASID

PT Unmap / Finalize:
  1. Check PT refcount == 0 (assert in debug, error in release)
  2. If refcount > 0: drain live leaves first (walk PT slots, invalidate each)
  3. Proceed with PT teardown only after refcount reaches 0

Untyped Reset / Retype:
  1. PT object reuse blocked while refcount > 0
  2. Generic resetUntypedCap path must check arch-specific reuse gate
```

## Implementation Plan

### Phase 0: Revert D-Series Experiments

Before starting the new implementation, cleanly revert the D-series
experimental commits to get back to a clean baseline. The D-series commits in
the kernel are:

```
03f5a0a92  arm64: experiment with pt liveness drain on pt teardown
e6c76c95e  arm64: retire mapped frames by slot identity
336551859  arm64: store frame mapping slot identity
e99309d5b  arm64: add pt liveness metadata scaffolding
7523f5b2b  arm64: experiment with full-ASID leaf retirement
c803b03c9  arm64: complete TLBI before VTTBR restore
57d3eb528  arm64: split first publication from leaf replacement
64c32ceff  arm64: factor live translation slot updates
```

Not all of these need reverting -- `64c32ceff` (factor live translation slot
updates) and `57d3eb528` (split first publication from leaf replacement) are
clean refactors that the new implementation can build on. Similarly,
`c803b03c9` (complete TLBI before VTTBR restore) is a standalone correctness
fix.

Revert list:
- `03f5a0a92` (pt liveness drain)
- `e6c76c95e` (retire by slot identity)
- `336551859` (store frame mapping slot identity)
- `e99309d5b` (pt liveness metadata scaffolding)
- `7523f5b2b` (full-ASID leaf retirement)

Keep:
- `64c32ceff` (factor live translation slot updates)
- `57d3eb528` (split first publication from leaf replacement)
- `c803b03c9` (TLBI before VTTBR restore)

### Phase 1: Cap Format Change

**Files:**
- `kernel/include/arch/arm/arch/64/mode/object/structures.bf`

**Work:**

1. Rename `capFMappedAddress` to `capFMappedSlotID` in the `frame_cap` block
2. Keep the same `field_high 48` declaration (bitfield generator produces
   identical shift/mask code)
3. All existing `cap_frame_cap_get_capFMappedAddress()` /
   `cap_frame_cap_set_capFMappedAddress()` accessors are automatically renamed
   to `cap_frame_cap_get_capFMappedSlotID()` /
   `cap_frame_cap_set_capFMappedSlotID()`

**New encoding/decoding helpers** (in `vspace.c` or a new `frame_mapping.h`):

```c
#define FRAME_SLOT_ID_INDEX_BITS  9
#define FRAME_SLOT_ID_BASE_SHIFT  FRAME_SLOT_ID_INDEX_BITS

/* Encode PT base + slot index into the 48-bit capFMappedSlotID field */
static inline word_t encodeFrameSlotID(pte_t *ptBase, word_t slotIndex)
{
    word_t base_pfn = pptr_to_paddr(ptBase) >> seL4_PageBits;
    return (base_pfn << FRAME_SLOT_ID_BASE_SHIFT) | (slotIndex & MASK(FRAME_SLOT_ID_INDEX_BITS));
}

/* Decode capFMappedSlotID into a direct slot pointer */
static inline pte_t *decodeFrameSlotID(word_t slotID)
{
    word_t base_pfn = slotID >> FRAME_SLOT_ID_BASE_SHIFT;
    word_t slotIndex = slotID & MASK(FRAME_SLOT_ID_INDEX_BITS);
    pte_t *ptBase = paddr_to_pptr(base_pfn << seL4_PageBits);
    return ptBase + slotIndex;
}

/* Get parent PT base from a slotID */
static inline pte_t *slotIDParentBase(word_t slotID)
{
    word_t base_pfn = slotID >> FRAME_SLOT_ID_BASE_SHIFT;
    return paddr_to_pptr(base_pfn << seL4_PageBits);
}
```

**Accessor update script:** A global rename of all call sites from
`capFMappedAddress` to `capFMappedSlotID`. The semantic meaning of the field
changes, but the accessor signature (`word_t` in, `word_t` out) does not.

### Phase 2: PT Refcount Table

**Files:**
- `kernel/include/arch/arm/arch/64/mode/model/statedata.h`
- `kernel/src/arch/arm/64/model/statedata.c`
- `kernel/src/arch/arm/64/kernel/vspace.c` (or new header)

**Work:**

Replace the D-series hash table with a minimal refcount table:

```c
/* statedata.h */
#define PT_REFCOUNT_TABLE_BITS  12
#define PT_REFCOUNT_TABLE_SIZE  BIT(PT_REFCOUNT_TABLE_BITS)

typedef struct {
    pte_t *pt;
    uint16_t leaf_count;
} pt_refcount_entry_t;

extern pt_refcount_entry_t armKSPTRefcountTable[PT_REFCOUNT_TABLE_SIZE];
```

32 KB total (4096 entries x 8 bytes). Half the entries could be enough for
most workloads (16 KB at 2048 entries).

API:

```c
void     ptRefcountInc(pte_t *ptBase);
void     ptRefcountDec(pte_t *ptBase);
uint16_t ptRefcountGet(pte_t *ptBase);
void     ptRefcountForget(pte_t *ptBase);
```

Open addressing with linear probing. No tombstones needed: `ptRefcountForget()`
is called only at PT teardown when count is zero, and can use backward-shift
deletion (Robin Hood style) to maintain probe chain integrity.

### Phase 3: Wire Frame Map Path

**Files:**
- `kernel/src/arch/arm/64/kernel/vspace.c`

**Work in `performPageInvocationMap()`:**

```c
static exception_t performPageInvocationMap(asid_t asid, cap_t cap, cte_t *ctSlot,
                                            pte_t pte, pte_t *ptSlot, vptr_t vaddr)
                                            /*                         ^^^^^ new param */
{
    bool_t was_valid = pte_ptr_get_valid(ptSlot);
    vm_page_size_t size = cap_frame_cap_get_capFSize(cap);
    pte_t *ptBase = (pte_t *)ROUND_DOWN((word_t)ptSlot, seL4_PageTableBits);
    word_t slotIndex = ptSlot - ptBase;

    /* Encode slot identity into the cap */
    cap = cap_frame_cap_set_capFMappedSlotID(cap, encodeFrameSlotID(ptBase, slotIndex));
    cap = cap_frame_cap_set_capFMappedASID(cap, asid);
    ctSlot->cap = cap;

    if (unlikely(was_valid)) {
        /* Remap: retire old, publish new */
        replaceTranslationSlotByASIDVA(ptSlot, pte, asid, vaddr);
    } else {
        /* First map: publish and increment refcount */
        publishTranslationSlot(ptSlot, pte);
        ptRefcountInc(ptBase);
    }

    return EXCEPTION_NONE;
}
```

Update the call site in `decodeARMFrameInvocation()` to pass `vaddr`:

```c
return performPageInvocationMap(asid, cap, cte,
                                makeUserPagePTE(base, vmRights, attributes, frameSize),
                                lu_ret.ptSlot, vaddr);
                             /* ^^^^^ vaddr passed explicitly from syscall arg */
```

### Phase 4: Wire Frame Unmap / Finalize Path

**Files:**
- `kernel/src/arch/arm/64/kernel/vspace.c`
- `kernel/src/arch/arm/64/object/objecttype.c`

**New `unmapPageBySlotID()` -- the core improvement:**

```c
void unmapPageBySlotID(cap_t cap)
{
    word_t slotID = cap_frame_cap_get_capFMappedSlotID(cap);
    if (slotID == 0) return;  /* not mapped */

    pte_t *ptSlot = decodeFrameSlotID(slotID);
    pte_t *ptBase = slotIDParentBase(slotID);
    pte_t pte = *ptSlot;
    pptr_t pptr = cap_frame_cap_get_capFBasePtr(cap);
    asid_t asid = cap_frame_cap_get_capFMappedASID(cap);

    /* Validate the slot still holds our mapping */
    if (!pte_is_page_type(pte)) return;
    if (pte_get_page_base_address(pte) != pptr_to_paddr((void *)pptr)) return;

    /* Retire: invalidate slot, clean cache, TLBI */
    *ptSlot = pte_pte_invalid_new();
    cleanInvalByVA((vptr_t)ptSlot, pptr_to_paddr(ptSlot));
    invalidateTLBByASID(asid);

    /* Decrement parent PT refcount */
    ptRefcountDec(ptBase);
}
```

**No `findVSpaceForASID()`. No `lookupPTSlot()`. No tree walk.** This is the
entire point.

**Update `performPageInvocationUnmap()`:**

```c
static exception_t performPageInvocationUnmap(cap_t cap, cte_t *ctSlot)
{
    if (cap_frame_cap_get_capFMappedASID(cap) != asidInvalid) {
        unmapPageBySlotID(cap);
    }

    cap_t slotCap = ctSlot->cap;
    slotCap = cap_frame_cap_set_capFMappedSlotID(slotCap, 0);
    slotCap = cap_frame_cap_set_capFMappedASID(slotCap, asidInvalid);
    ctSlot->cap = slotCap;

    return EXCEPTION_NONE;
}
```

**Update `Arch_finaliseCap()` for frame caps:**

```c
case cap_frame_cap:
    if (cap_frame_cap_get_capFMappedASID(cap) != asidInvalid) {
        unmapPageBySlotID(cap);
    }
    break;
```

**Keep old `unmapPage()` as a fallback** for any edge case or legacy path that
still needs vaddr-based unmap (boot caps, IO space). It remains available but
is no longer the primary unmap path.

### Phase 5: Wire PT Teardown / Reuse Gate

**Files:**
- `kernel/src/arch/arm/64/kernel/vspace.c`
- `kernel/src/arch/arm/64/object/objecttype.c`
- `kernel/src/object/untyped.c` (arch hook)

**PT Teardown in `Arch_finaliseCap()`:**

```c
case cap_page_table_cap:
    if (final && cap_page_table_cap_get_capPTIsMapped(cap)) {
        pte_t *pt = PT_PTR(cap_page_table_cap_get_capPTBasePtr(cap));

        /* Drain any remaining live leaves before teardown */
        if (ptRefcountGet(pt) > 0) {
            drainPTLiveLeaves(pt, cap_page_table_cap_get_capPTMappedASID(cap));
        }
        assert(ptRefcountGet(pt) == 0);

        unmapPageTable(cap_page_table_cap_get_capPTMappedASID(cap),
                       cap_page_table_cap_get_capPTMappedAddress(cap),
                       pt);
        ptRefcountForget(pt);
    }
    if (final) {
        sanitiseTranslationObject(pt, seL4_PageTableBits);
    }
    break;
```

**`drainPTLiveLeaves()`:** Walk all 512 slots of the PT, invalidate any that
are still valid leaf entries, decrement the refcount for each. This handles the
case where frame caps pointing into this PT have already been deleted but the
PT still has live entries (e.g., during bulk process destruction where PT
teardown can race ahead of frame cap finalization).

**Untyped reset gate:** Add an arch hook in `resetUntypedCap()` that checks
whether any translation object in the region being reset has a nonzero
refcount. If so, drain before proceeding.

### Phase 6: Update Secondary Consumers

**Files:**
- `kernel/src/arch/arm/64/kernel/vspace.c` (flush paths)
- `kernel/src/arch/arm/64/machine/capdl.c` (debug)

**Page flush (EL2 path):**

The EL2 flush path at
[vspace.c:1949](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1949)
uses physical addresses directly (`pptr_to_paddr(capFBasePtr) + start`). It
reads `vaddr` only for the `performPageFlush()` call. Under EL2, the flush
implementation uses physical addresses for cache maintenance. The vaddr
parameter is only used for the TLBI component.

Two options:
1. Reconstruct vaddr via tree walk (acceptable for infrequent flush path)
2. Use ASID-wide TLBI instead of VA-targeted TLBI for flushes (simpler, small
   performance cost on flush -- which is already expensive)

Recommend option (2) for initial implementation, with option (1) as a later
optimization if flush performance matters.

**Page flush (EL1 path):**

The EL1 path at
[vspace.c:1960](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1960)
already does its own `lookupPTSlot()` to validate the mapping is not stale.
It can derive vaddr from that walk. No change needed beyond removing the
`armFrameCapGetMappedVaddr()` call and using the syscall-provided vaddr context
instead.

**Remap check:**

The remap check at
[vspace.c:1869](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c#L1869)
currently compares slot identity via `armFrameCapGetMappedSlot()`. With the new
cap format this becomes a direct comparison:

```c
word_t storedSlotID = cap_frame_cap_get_capFMappedSlotID(cap);
word_t targetSlotID = encodeFrameSlotID(
    ROUND_DOWN(lu_ret.ptSlot, seL4_PageTableBits),
    lu_ret.ptSlot - ROUND_DOWN(lu_ret.ptSlot, seL4_PageTableBits));

if (storedSlotID != targetSlotID) {
    /* remap to different address -- reject */
}
```

**capDL debug:**

[capdl.c:314](/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/machine/capdl.c#L314)
currently calls `armFrameCapGetMappedVaddr()`. Replace with a tree-walk
reconstruction. This is debug-only code and performance does not matter.

### Phase 7: Clean Up

1. Remove all D-series code:
   - `arm_pt_liveness_entry_t`, `armKSPTLivenessTable`
   - `armPTLivenessHash`, `armPTLivenessLookup`, `ARM_PT_LIVENESS_TOMBSTONE`
   - `armPTLivenessEnsureTracked`, `armPTLivenessForget`,
     `armPTLivenessMaybeForget`, `armPTLivenessMarkMapped`,
     `armPTLivenessMarkUnmapped`, `armPTLivenessInc`, `armPTLivenessDec`,
     `armPTLivenessGet`, `armPTLivenessGetMapped`
   - `ARM_FRAME_MAP_ID_TAG`, `ARM_FRAME_MAP_SLOT_BITS`,
     `ARM_FRAME_MAP_PT_BASE_BITS`
   - `armFrameCapSetMappedIdentity`, `armFrameCapGetMappedSlot`,
     `armFrameCapGetMappedVaddr`, `armFrameCapUnmapByIdentity`
   - `retireFrameSlotByIdentity`, `retireTranslationSlotByASIDVAFullFlush`

2. Remove the tag-bit dual encoding and legacy fallback paths

3. Verify no other code references the removed symbols

Estimated removal: ~600-700 lines of experimental code.

## Commit Series

```
R1: revert D-series experimental commits (except clean refactors)
C1: structures.bf: rename capFMappedAddress to capFMappedSlotID
C2: vspace: add slot identity encoding/decoding helpers
C3: vspace: add minimal PT refcount table
C4: vspace: encode slot identity on frame map
C5: vspace: add unmapPageBySlotID (direct slot retirement)
C6: vspace: wire unmap/finalize to use unmapPageBySlotID
C7: objecttype: gate PT teardown on refcount drain
C8: vspace: update flush paths for new cap format
C9: capdl: update debug dump for new cap format
C10: clean up: remove remaining D-series scaffolding
```

## What This Achieves

| Property | D-Series | New Design |
|----------|----------|------------|
| Slot identity in cap | Tag-bit hack in capFMappedAddress | Native cap field |
| PT liveness tracking | 512 KB hash table (32 bytes/entry) | 32 KB table (8 bytes/entry) |
| Vaddr reconstruction | Hash table lookup + arithmetic | Not needed on hot path |
| Legacy fallback | Tag-bit discriminated dual path | None (clean cut) |
| unmapPage() tree walk | Still present as fallback | Eliminated on primary path |
| Code complexity | ~700 lines of experimental code | ~200 lines of clean implementation |

## Risks

1. **Boot-created frame caps.** The kernel boot path creates frame caps with
   `capFMappedAddress = vaddr`. These must be converted to use slot identity
   encoding. The boot path already knows the PT slot when it maps, so this is
   straightforward.

2. **IO space frames.** ARM SMMU / IO space code at
   `kernel/src/arch/arm/object/iospace.c` sets `capFMappedAddress` for IO
   frames. IO frames follow a different mapping path and may not have a
   conventional PT slot. These may need a separate encoding or remain
   vaddr-based with a discriminator.

3. **Other architectures.** This plan only changes ARM64. x86 and RISC-V have
   the same structural `unmapPage()` design deficiency but different PA widths
   and PT formats. The encoding constants would differ. Other architectures are
   explicitly out of scope for this initial implementation.

4. **Formal verification.** Changing `structures.bf` invalidates all generated
   Isabelle/HOL proofs for the ARM64 cap format. This is acceptable for the
   current experimental/Orin-focused work but would need proof regeneration
   before upstream submission.

5. **HugePage (1 GB) parent identity.** On configs with
   `CONFIG_AARCH64_VSPACE_S2_START_L1`, huge pages are mapped at L1. Their
   "parent" is the vspace root, not a regular PT. The encoding must handle this
   (use vspace root base as the PT base, with the L1 index as slot index).

## ARM Architectural Violations In Upstream Baseline

Independent of the D-series work and the cap-format redesign, the upstream
vanilla seL4 ARM64 vspace code has several places where it does not meet ARM
Architecture Reference Manual requirements. The cap-format redesign naturally
fixes these because structuring the code around explicit lifecycle phases forces
correct sequencing.

This section documents the violations against the upstream pre-D-series
baseline, referencing the ARM ARM (DDI 0487) and the corresponding Linux/KVM
analogues.

### 1. No Break-Before-Make on valid-to-valid remap

The upstream `performPageInvocationMap()` does a direct valid-to-valid
descriptor overwrite:

```c
/* upstream baseline */
bool_t tlbflush_required = pte_ptr_get_valid(ptSlot);
*ptSlot = pte;                                          /* direct overwrite */
cleanInvalByVA((vptr_t)ptSlot, pptr_to_paddr(ptSlot));
if (unlikely(tlbflush_required)) {
    invalidateTLBByASIDVA(asid, vaddr);
}
```

This violates the ARM ARM Break-Before-Make (BBM) requirement (D8.14.1). The
architecture requires that when changing a valid translation table entry to
another valid entry with different attributes or output address:

1. Write an invalid entry to the slot
2. `DSB ISH` to ensure the invalid write is observable
3. `TLBI` to remove stale TLB entries
4. `DSB ISH` to ensure TLBI completion
5. Write the new valid entry

seL4 skips steps 1-4 and does a direct overwrite followed by a post-hoc TLBI.
On simpler cores this may work by accident. On Orin's complex out-of-order
interconnect, it provably triggers TLB conflict aborts (the hardware walker
sees both the old and new entry simultaneously).

The D-series `replaceTranslationSlotByASIDVA()` partially fixes this by doing
invalidate-first-then-publish, but even that lacks the required `DSB ISH`
between the TLBI and the new publication.

**Linux analogue:** KVM stage-2 code in
[kvm/hyp/pgtable.c](/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/kvm/hyp/pgtable.c#L627)
explicitly clears the old entry, performs `dsb(ishst)`, TLBI, `dsb(ish)`,
before installing a new valid entry.

**How the redesign fixes this:** With direct slot identity in the cap, the
remap path knows exactly which slot it is replacing without re-walking. It can
implement proper BBM as a clean sequence:

```c
/* invalidate old */
WRITE_ONCE(*ptSlot, pte_pte_invalid_new());
dsb(ishst);
tlbi_by_asid_va(asid, vaddr);
dsb(ish);
/* publish new */
WRITE_ONCE(*ptSlot, new_pte);
dsb(ishst);
```

### 2. No walker-visibility barrier on new PT publication

The upstream `performPageTableInvocationMap()`:

```c
ctSlot->cap = cap;
*ptSlot = pte;
cleanInvalByVA((vptr_t)ptSlot, pptr_to_paddr(ptSlot));
```

This publishes a parent-table descriptor pointing to a newly created child PT.
But there is no guarantee that the child PT's zeroed contents are visible to the
hardware page table walker before the parent descriptor goes live.

The child was zeroed by `sanitiseTranslationObject()` during
`Arch_createObject()` (which does `cleanInvalidateCacheRange_RAM()`). But
between that point and parent-link publication, there is no `DSB ISH` to ensure
the zeroed state is observable by the walker at the moment the parent link
becomes valid.

The architecture permits the walker to speculatively fetch through a
newly-valid table descriptor. If the child page's zeroed state has not been made
coherent, the walker can observe stale data and construct a bogus translation.

**Linux analogue:**
[mmu.c:400](/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/mm/mmu.c#L400)
explicitly does `dsb(ishst)` after allocating a zeroed page-table page, with
the comment: *"Ensure the zeroed page is visible to the page table walker."*

KVM does the same in
[kvm/hyp/pgtable.c:1136](/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/kvm/hyp/pgtable.c#L1136).

**How the redesign fixes this:** The new `performPageTableInvocationMap()`
naturally separates child preparation from parent publication, with an explicit
walker-visibility barrier between them.

### 3. Plain C stores for descriptor writes

Every descriptor write in upstream seL4 is a plain `*slot = pte` — an anonymous
C store with no ordering semantics. The compiler is free to reorder, split, or
coalesce these writes.

A 64-bit PTE written as `*slot = pte` where `pte` is a struct can be compiled
as two 32-bit stores by some compilers at certain optimization levels. This is
architecturally undefined for a translation table entry that the hardware walker
can read concurrently. The walker can observe a half-written entry where bits
`[63:32]` and bits `[31:0]` are from different descriptors.

**Linux analogue:** Linux uses `WRITE_ONCE()` for all PTE writes to prevent
compiler tearing, and `smp_store_release()` where ordering against prior stores
matters.

**How the redesign fixes this:** The new `publishTranslationSlot()` uses an
explicit single-copy-atomic store primitive, making the intent explicit and
compiler-safe.

### 4. No DSB between descriptor invalidation and TLBI

The upstream retire path in `unmapPage()`:

```c
*(lu_ret.ptSlot) = pte_pte_invalid_new();
cleanInvalByVA((vptr_t)lu_ret.ptSlot, pptr_to_paddr(lu_ret.ptSlot));
invalidateTLBByASIDVA(asid, vptr);
```

The `cleanInvalByVA()` is a cache maintenance operation (`dc civac`), not a
barrier. The ARM ARM requires a `DSB` between the store that makes an entry
invalid and the subsequent TLBI, to ensure the invalid entry is observable
before TLB maintenance takes effect. `DC CIVAC` pushes data toward PoC but
does not serve as an ordering barrier for the TLB maintenance sequence.

The required sequence per ARM ARM (D8.14.2) is:

```
STR invalid      -- make entry invalid
DSB ISH           -- ensure invalid is observable
TLBI              -- invalidate TLB
DSB ISH           -- ensure TLBI complete
```

seL4 does `STR; DC CIVAC; TLBI` — missing both DSBs.

**How the redesign fixes this:** The new `unmapPageBySlotID()` implements the
correct `store-DSB-TLBI-DSB` sequence because the direct slot access makes the
retire path a simple, auditable sequence with no tree-walk complexity obscuring
the barrier requirements.

### 5. `clearMemory()` for translation objects during reset

`resetUntypedCap()` at
[untyped.c:234](/home/hlyytine/tii-sel4/kernel/src/object/untyped.c#L234)
calls `clearMemory()`, which is just `memzero()` with zero cache maintenance.
If the region being reset previously held a page-table object, the zeroed state
is never made cache-coherent before later reuse via `Arch_createObject()`.

The creation path does `sanitiseTranslationObject()` (which includes
`cleanInvalidateCacheRange_RAM()`), so the fresh object is eventually
sanitized. But between `clearMemory()` zeroing and `sanitiseTranslationObject()`
re-sanitizing, there is a window where the memory holds bare-zeroed-but-not-
cache-maintained data. If a stale TLB entry still points through the old page
table (because TLBI was not completed before reset), the walker can traverse
this half-maintained state.

**How the redesign fixes this:** The PT refcount gate ensures that a PT object
cannot be reset/reused while any frame cap still references slots within it.
This eliminates the window entirely — by the time `resetUntypedCap()` runs,
all live references have been retired and their TLBIs completed.

### 6. No completion barrier after TLBI before reuse

After `unmapPage()` retires a leaf entry and issues TLBI, the kernel returns to
the caller. Later, the same physical frame or the parent PT can be reused. But
there is no `DSB ISH` after the TLBI to ensure it has **completed** before the
memory is repurposed. Per ARM ARM, a TLBI is only guaranteed complete after a
subsequent `DSB`. Without that, the old TLB entry can still be in-flight while
the memory it referenced is already being rewritten.

**How the redesign fixes this:** The new `unmapPageBySlotID()` includes a
completion `DSB ISH` after TLBI, and the PT refcount decrement happens only
after that barrier. This means the refcount reaching zero is a reliable signal
that all TLBIs for all leaves in that PT have completed, and the PT is safe to
reuse.

### Summary: Why The Redesign Naturally Produces Correct Code

The current upstream code conflates discovery (walking the tree to find a slot)
with mutation (writing/invalidating that slot). This makes the barrier
requirements hard to reason about because the walk itself is reading live
descriptors that the mutation is changing.

The cap-format redesign separates these concerns:

- **Discovery is done at map time** (the `lookupPTSlot()` during
  `decodeARMFrameInvocation()`) and the result is stored in the cap
- **Mutation at unmap time** goes directly to the known slot

This separation means the retire/publish sequences become short, straight-line
code with obvious barrier insertion points. The ARM-mandated `store-DSB-TLBI-DSB`
sequence falls out naturally because there is no interleaved walk logic to
complicate the ordering.

| Upstream baseline | Redesigned |
|-------------------|-----------|
| Walk + mutate interleaved | Walk at map, mutate by identity at unmap |
| Barrier placement obscured by walk logic | Barrier placement obvious in linear sequence |
| BBM violated on remap | BBM naturally structured |
| No walker-visibility DSB | DSB between child zero and parent publish |
| Plain C stores | Explicit single-copy-atomic stores |
| TLBI without completion DSB | TLBI + completion DSB before refcount dec |
| No reuse gate | Refcount gate ensures TLBI complete before reuse |

## Disposition Of Local FOR-UPSTREAM Commits

The local branch carries several FOR-UPSTREAM fixes on top of the upstream base
(`717cf90201f5`). These need individual assessment: some are independently
correct and should be kept regardless of the redesign, others are
symptom-targeted workarounds that the redesign is expected to supersede.

### Keep unconditionally

These are genuine upstream bugs, orthogonal to the cap-format redesign:

**`f6aa6669d` — Split cache.c into ARM32 and ARM64 versions**

The upstream cache.c uses the ARM32 L2C-310 external controller API
(`plat_cleanL2Range`, etc.) for ARM64, which is architecturally nonsensical.
ARM64 has no external L2 controllers — `dc civac` handles all levels. The
rewrite also adds proper `dsb ishst` / `dsb ish` barriers around cache
maintenance loops, matching Linux `arch/arm64/mm/cache.S`.

The upstream `cleanInvalByVA()` was bare `dc civac; dsb sy` with no
pre-barrier. The fixed version adds `dsb ishst` before to ensure prior stores
are visible before maintenance. Correct per ARM ARM.

**`0d5cee148` — Use valid PT base during VMID switch for TLB invalidation**

The upstream code sets `VTTBR_EL2` with `base=0` during VMID switches for TLB
invalidation. The ARM ARM does not guarantee that speculative table walks won't
fire against VTTBR during the TLBI window. On Orin, they do, and PA=0 lands in
device/reserved space causing RAS errors. Using `armKSGlobalUserVSpace` (an
empty, valid page table) as the base is what Xen does for the same reason.
This is a real upstream bug affecting any platform with speculative walkers
during TLBI.

**`8a282fba1` — Fix AARCH64_VSPACE_S2_START_L1 config**

The CMake used the wrong variable name (`ARM_HYPERVISOR_SUPPORT` instead of
`KernelArmHypervisorSupport`), and C code used `AARCH64_VSPACE_S2_START_L1`
without the `CONFIG_` prefix. On 40-bit PA platforms the option was silently
never enabled, producing incorrect 4-level page tables where 3-level is
correct. Build-system bug, completely orthogonal to the cap-format work.

### Expected to become unnecessary after redesign

These three commits form a group that targets the **symptom** (speculative
walker hits PA=0 from invalid PTE padding bits) rather than the **cause**
(descriptors still appear valid to the walker because retirement sequencing is
wrong). After the redesign with correct `DSB-TLBI-DSB` sequencing and reuse
gating, the walker should never see a valid-looking descriptor for memory that
has been repurposed, and invalid descriptors should never be speculatively
followed because the TLBI is architecturally complete before anyone can reuse
the memory.

**However, these should not be deleted permanently.** If the cap-format
redesign fails to fully eliminate the Orin RAS errors, these workarounds remain
the best available symptom-level mitigation. They should be reverted from the
active branch but preserved as named commits (or documented here) so they can
be re-applied quickly.

**`136478271` — Override pte_pte_invalid_new() with safe PA for speculative PTW**

Overrides the bitfield-generated `pte_pte_invalid_new()` with a macro that
stuffs `armKSGlobalUserVSpace`'s PA into bits `[47:12]` of otherwise-invalid
PTEs, so speculative walkers that chase those bits don't fault on PA=0.

Concerns with this approach:
1. The ARM ARM says `bits[1:0]=0b00` means invalid; a conforming walker must
   stop. If Orin's walker chases PA bits from invalid entries, that is
   implementation errata, not a software correctness issue.
2. Global macro override of a bitfield accessor -- any code path that depends
   on `pte_pte_invalid_new()` producing zero will break silently.
3. Turns a compile-time constant (`mov x0, #0`) into a runtime memory load
   from `armKSGlobalUserVSpace`.

**`efdce541a` — Initialize armKSGlobalUserVSpace with safe invalid PTEs**

Fills the global empty vspace root with the "safe invalid" PTEs. Only needed
if `136478271` is kept — without the safe-PA override, the BSS-zeroed
`armKSGlobalUserVSpace` already contains architecturally correct all-zero
invalid entries.

**`1bf44e557` — Initialize VSpace/PageTable objects with safe invalid PTEs**

Adds safe-invalid initialization to `Arch_createObject()` for VSpace and PT
objects. Only needed if `136478271` is kept — the existing
`sanitiseTranslationObject()` already writes `pte_pte_invalid_new()` and does
`cleanInvalidateCacheRange_RAM()`.

### Orthogonal / keep per taste

**`1718a85f7` — kernel/boot: Add option for verbose debug**

Debugging aid, not related to the redesign. Keep or drop independently.

### Fallback plan

If the cap-format redesign does not fully resolve the Orin RAS errors after
implementation:

1. Re-apply `136478271` + `efdce541a` + `1bf44e557` (safe-PA invalid PTE
   group) on top of the redesigned code
2. This provides defense-in-depth: the redesign closes the retirement/reuse
   window, and the safe-PA workaround handles any residual speculative walker
   behavior that the correct sequencing alone does not suppress
3. The two approaches are not mutually exclusive — they address different
   levels of the problem (architectural sequencing vs implementation errata)

## Implementation Results

### Branch and commits

Implementation branch: `frame-cap-slot-identity`, based on `c803b03c9`.

Commit 1 (`7c1929058` / amended through `d6a90b206`):
**arm64: frame cap slot identity redesign**
- Frame caps store packed slot identity in `capFMappedAddress`
- PT refcount table (32 KB) for leaf-count tracking
- `unmapPageBySlotID()` for direct slot retirement without tree walk
- `drainPTLiveLeaves()` for PT teardown
- Volatile PTE stores, `dsb_ishst` before PT publication, `dsb(sy)` after
  TLBI in retire helpers, `cleanInvalidateCacheRange_RAM` in `clearMemory()`
- Boot-time frame caps use slotID=0 (boot mappings managed separately)
- 7 files changed, ~376 insertions, ~58 deletions

Commit 2 (`bf0a0f746`):
**arm64: store parent slot identity in page table caps**
- Extended `page_table_cap` with `capPTParentHigh` (10 bits) and
  `capPTParentLow` (20 bits) using existing padding — 30 bits total
- Encoding: 21-bit PFN (4K-aligned, covers 8GB PA) + 9-bit slot index
- `Arch_finaliseCap` for PT uses stored identity to directly invalidate
  the parent table descriptor when the ASID is already deleted
- Fixes the last remaining ARM violation: stale parent descriptors in
  live page table pages after bulk cap revocation
- 4 files changed, ~80 insertions, ~10 deletions

### Validation runs

| Run | Image | RAS errors | Tests | Key finding |
|-----|-------|-----------|-------|-------------|
| `20260315-234355` (v2) | frame cap only | 0 (ASID assert on test 134) | 120 passed | Slot identity works, ASID-gone path needs guard |
| `20260315-235502` (v3 retry) | + ASID guard | 10 | 141 passed, suite passed | All tests pass, RAS in CANCEL_BADGED_SENDS_0002 |
| `20260316-000254` (v4) | + volatile store, drain TLBI, dsb_ishst | 2 | 141 passed | Barrier improvements reduce RAS count |
| `20260316-000722` (v5) | + clearMemory cache flush | 6 | 141 passed | Non-deterministic, clearMemory not the main cause |
| `20260316-001504` (v6) | + TLBI-all for orphaned PT | 4 | 141 passed | TLBI-all helps but doesn't fix root cause |
| `20260316-003226` (v7) | + dsb(sy) after all TLBI | 0 | 141 passed | First zero-RAS run (lucky?) |
| `20260316-002056` (v7 retry) | same | 2 | 141 passed | Confirmed non-deterministic residual |
| **`20260316-104524`** (v8) | **+ PT parent slot identity** | **0** | **141 passed** | **Clean pass — parent descriptor fix eliminates kernel residual** |
| `20260316-105033` (v8 retry) | same | RAS in elfloader | n/a | Elfloader/UEFI handoff issue, not kernel |
| **`20260316-120426`** (v9) | **+ elfloader quiesce + kernel head.S TLBI** | **0** | **141 passed** | **Zero RAS end-to-end — all boot + runtime errors eliminated** |
| **`20260316-120852`** (v9 retry) | same | **0** | **141 passed** | **Confirmed reproducible** |

### Root cause confirmed

The Orin RAS errors had two distinct root causes in the seL4 kernel:

1. **Frame cap unmapPage() tree re-walk trust** (dominant): `unmapPage()` was
   forced to re-walk the live translation tree to find the leaf slot because
   frame caps only stored the mapped virtual address, not the slot identity.
   Fixed by storing direct slot identity in frame caps.

2. **Stale parent table descriptors after bulk teardown** (residual): When
   `deleteASID()` runs before PT finalization during cap revocation,
   `unmapPageTable()` cannot find the vspace root and bails without
   invalidating the parent descriptor. The stale descriptor remains in a live
   page table, and the hardware walker follows it into freed/reused memory.
   Fixed by storing parent slot identity in PT caps and directly invalidating
   the parent descriptor at finalization time.

### Resolved: elfloader/UEFI RAS errors

The elfloader/UEFI RAS errors have been fixed by two additional commits
following Linux's `efi-entry.S` → `head.S` pattern:

**Elfloader commit (`a2ebe05`):** `quiesce_uefi_hyp()` — immediately after
ExitBootServices, flush D-cache to PoC, disable EL2 MMU (stopping the
hardware walker), then TLBI alle2is (safe with walker stopped). The
elfloader continues MMU-off until `arm_enable_hyp_mmu()`.

**Kernel commit (`12366f1cd`):** TLBI alle2is + IC IALLUIS + DSB + ISB at the
top of `_start` in head.S, before any SCTLR configuration. Flushes stale
elfloader TLB entries inherited from the previous boot stage.

The key insight (from studying Linux's approach): **disable MMU first, then
TLBI.** An earlier attempt to do TLBI with MMU still on caused a worse RAS
flood (81 errors) because the hardware re-walked through UEFI's stale tables
during the invalidation itself.

Investigation of ATF and UEFI sources confirmed:
- ATF sets VTTBR_EL2=0, HCR_EL2.VM=0 (no stage-2 translation involved)
- UEFI never touches VTTBR_EL2
- UEFI does NOT perform any TLBI or page table cleanup during
  ExitBootServices
- The stale entries were from EL2 stage-1 (TTBR0_EL2) mappings of
  reclaimed boot-services heap

Validation: two consecutive runs with zero RAS errors end-to-end (boot
through all 141 sel4test tests).

## Non-Goals

- Changing other architectures (x86, RISC-V, ARM32)
- Upstream submission or proof regeneration
- Adding reverse-map metadata beyond a simple refcount
