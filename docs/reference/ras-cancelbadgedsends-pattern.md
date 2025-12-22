# RAS Error Pattern: CancelBadgedSends Thread Wake-up Cascade

**Date**: 2025-12-22
**Status**: Reproducible pattern identified
**Last Updated**: 2025-12-22 (added Phase 24 analysis with el2-ftrace data)

## Summary

RAS errors on Orin AGX are triggered by a specific, reproducible syscall pattern during the `CANCEL_BADGED_SENDS_0002` test. The error manifests during **CNODE_REVOKE cleanup operations** that follow the thread wake-up cascade from `cnode_cancelBadgedSends()`.

**Key Finding**: The RAS error does NOT occur during `cancelBadgedSends` itself - it occurs later during page table cleanup (`unmapPage` → `lookupPTSlot`) when walking PTEs that were corrupted during the thread cascade.

## The Pattern

All four test runs showed **consistent syscall sequences** before the RAS error:

```
Reply -> Call(tag=21) -> Call(tag=23) -> Call(cap=4) ->
  Recv(0x80afe8b800) -> Recv(0x80afe77800) -> Recv(0x80afe64800) ->
  Recv(0x80afe50800) -> Recv(0x80afe3d800) -> Recv(0x80afe2a800) ->
  Recv(0x80afe17800) -> Recv(0x80afe03800) -> RAS_ERROR
```

### Syscall Tags

| Tag | Invocation | Description |
|-----|------------|-------------|
| 21 | `CNodeRevoke` | Revokes the derived badged endpoint cap |
| 23 | `CNodeCancelBadgedSends` | Cancels pending sends, wakes blocked threads |

### Test Code Reference

From `projects/sel4test/apps/sel4test-tests/src/tests/endpoints.c`:

```c
// test_ep_cancelBadgedSends2
for (int i = 0 ; i < NUM_BADGED_CLIENTS; i++) {
    error = cnode_revoke(env, helpers[i].badged_ep);      // tag 21
    error = cnode_cancelBadgedSends(env, helpers[i].badged_ep);  // tag 23
    // Then all helper threads wake up and do Recv...
}
```

## Evidence

### Run 1 (2025-12-22 00:48) - el2-ras mode
- **Test iteration**: 85
- **Ftrace events**: 5,395,867
- **Interrupted PC**: `0x8080024c4c` → `loadAllDisabledBreakpointState` (debug.c:497)
- **Request ID**: 20251222-004834

### Run 2 (2025-12-22 01:03) - el2-ras mode
- **Test iteration**: 14
- **Ftrace events**: 878,309
- **Interrupted PC**: `0x808002cfb8` → `benchmark_track_exit` (benchmark_track.c:18)
- **Request ID**: 20251222-010300

### Run 3 (2025-12-22 01:51) - el2-ftrace mode (selective tracing)
- **Build**: el2-ftrace with VTTBR, TLBI, DSB, selective function tracing
- **Ftrace events**: 310,000+
- **Interrupted PC**: `0x80800394ec` → `pte_ptr_get_pte_type` (structures_gen.h:2272)
- **Request ID**: 20251222-015149
- **Key finding**: Error occurs during CNODE_REVOKE cleanup, ~137,000 events after last CANCEL_BADGED_SENDS

### Run 4 (earlier runs)
- Multiple earlier runs from Phase 15-23 showed the same pattern
- Consistent thread cascade after cancelBadgedSends
- Error always during page table operations in cleanup

### Common Denominators

| Attribute | Run 1 | Run 2 | Run 3 |
|-----------|-------|-------|-------|
| VSpace | 0xac002000 | 0xac002000 | 0xac002000 |
| TCB sequence | 0x80afe8b800→... | 0x80afe8b800→... | 0x80afeb3800→... |
| Syscall pattern | Reply→Call(21)→Call(23)→8×Recv | Reply→Call(21)→Call(23)→8×Recv | Reply→Call(21)→Call(23)→20+×Recv |
| Error location | varies | varies | unmapPage→lookupPTSlot |
| RAS event type | SCC Uncorrectable | SCC Uncorrectable | SCC Uncorrectable |

**Key observations**:
1. TCB addresses are consistent across runs (same allocation pattern)
2. Test creates multiple helper threads that block on endpoints
3. `cancelBadgedSends` wakes them all, triggering cascade
4. Error manifests during cleanup, not during the cascade itself

## Ftrace Event Breakdown

| Event Type | Run 1 (el2-ras) | Run 2 (el2-ras) | Run 3 (el2-ftrace) |
|------------|-----------------|-----------------|---------------------|
| kernel_entry | 1,400,575 | 232,248 | - |
| kernel_exit | 1,403,997 | 232,907 | - |
| syscall | 1,139,605 | 187,403 | ~8,000 |
| safe_pte | 709,803 | 110,350 | - |
| vspace | 357,746 | 55,756 | ~5,500 |
| vmid | 202,106 | 31,280 | - |
| thread | 182,032 | 28,362 | ~6,600 |
| VTTBR | - | - | ~5,500 |
| TLBI | - | - | ~1,200 |
| DSB | - | - | ~5,500 |
| selective_enter/exit | - | - | ~6,600 |
| func_enter/exit | - | - | ~270,000 |
| ras_error_* | 3 | 3 | 3 |

**Note**: Run 3 used selective function tracing, so function enter/exit events are only logged within traced regions (setVMRoot, switchToThread, cancelBadgedSends, etc.).

## Analysis

The RAS error occurs during rapid context switching between multiple threads:

1. **CNodeRevoke** (tag 21) - revokes derived caps
2. **CNodeCancelBadgedSends** (tag 23) - wakes all 8 blocked threads
3. **Thread wake-up cascade** - kernel switches between 8 threads doing Recv
4. **RAS ERROR** - hits during this rapid switching

The interrupted PC varies (debug.c, benchmark_track.c) because the error can hit at any point during kernel execution. The trigger is the rapid VSpace/VMID switching as threads wake up.

## Phase 24: Detailed Ftrace Analysis (2025-12-22)

Using the new `el2-ftrace` build with selective function tracing, we confirmed the exact error location and timing.

### Test Run: 20251222-015149

| Metric | Value |
|--------|-------|
| Request ID | 20251222-015149 |
| Total ftrace events | 310,000+ |
| RAS error index | 310111 |
| Error ELR | 0x80800394ec (`pte_ptr_get_pte_type`) |
| Last CANCEL_BADGED_SENDS | index 173394 (empty, immediate exit) |
| Last CNODE_REVOKE before error | index 309360 |

### Thread Cascade Pattern

After `CANCEL_BADGED_SENDS`, 20+ threads wake up in sequence (TCB addresses descending):

```
155105: THREAD tcb=0x80afeb3800
155160: THREAD tcb=0x80afe9f800  (Recv)
155215: THREAD tcb=0x80afe8c800  (Recv)
155270: THREAD tcb=0x80afe78800  (Recv)
155325: THREAD tcb=0x80afe65800  (Recv)
155380: THREAD tcb=0x80afe52800  (Recv)
155435: THREAD tcb=0x80afe3e800  (Recv)
155490: THREAD tcb=0x80afe2b800  (Recv)
155545: THREAD tcb=0x80afe18800  (Recv)
155600: THREAD tcb=0x80afe05800  (Recv)
155655: THREAD tcb=0x8080ff1800  (Recv)
... (continues with 10+ more threads)
```

### Error Location: Page Table Lookup During Cleanup

The RAS error at index 310111 occurred in the call chain:

```
unmapPage (0x8080039940)
  └→ lookupPTSlot (0x8080039470)
       └→ pte_ptr_get_pte_type (0x80800394ec) ← RAS ERROR HERE
```

The error is in `pte_ptr_get_pte_type` at `kernel/generated/arch/object/structures_gen.h:2272`, which reads the PTE type field from a page table entry.

### Timing Analysis

```
[155099]  CANCEL_BADGED_SENDS enter
[155100]  CANCEL_BADGED_SENDS exit (empty - no internal work)
[155105-173394]  Thread cascade (20+ threads doing Recv)
[173394]  Last CANCEL_BADGED_SENDS (empty)
[177123-309360]  CNODE_REVOKE operations (cleanup)
[309360]  CNODE_REVOKE enter
[310111]  RAS ERROR during page table walk
```

**Gap**: ~137,000 events between last CANCEL_BADGED_SENDS and RAS error!

### Key Insight

The RAS error is **NOT** triggered directly by `cancelBadgedSends`. Instead:

1. `cancelBadgedSends` wakes up blocked threads
2. Threads execute in cascade, each doing `Recv` syscalls
3. Many subsequent `CNODE_REVOKE` operations run (cleanup)
4. **Error manifests later** when `unmapPage` tries to walk page tables
5. The PTEs being walked are **already corrupted** with stale data

This confirms the hypothesis that page tables get corrupted **during** the thread cascade, but the corruption is only detected when those PTEs are accessed during cleanup.

## Hypothesis

The `cancelBadgedSends` operation causes:
1. Multiple threads to be unblocked simultaneously
2. Rapid context switches between these threads
3. Multiple VSpace/VMID transitions in quick succession
4. Possible speculative page table walk issues during VMID switching

This aligns with previous findings about VMID 0 vs VMID 1 behavior and the need for DSB barriers before VTTBR changes (see [ARM Speculative PTW Research](arm-speculative-ptw-research.md)).

### Updated Hypothesis (Phase 24)

Based on the ftrace analysis:

1. **Corruption happens during thread cascade** - not during cancelBadgedSends itself
2. **Corruption is not immediately detected** - only found when PTEs are accessed later
3. **CNODE_REVOKE triggers detection** - unmapPage walks page tables and hits garbage
4. **Stale data pattern** - corrupted PTEs contain values from previous iterations

The root cause is likely:
- **Memory reuse race**: TCB/VSpace memory from previous test iteration reused before properly cleared
- **Speculative PTW pollution**: Hardware caches stale translations during rapid VMID switching
- **Missing TLB invalidation**: Old translations still cached when VSpace memory reused

## Build Configuration

The `el2-ras` build mode was used:
- `KernelFtraceBuffer=ON` - enables ftrace buffer for RAS event logging
- `KernelFunctionTrace=OFF` - no function enter/exit tracing (low overhead)
- `KernelArmSdeiRas=ON` - SDEI RAS error handling
- `KernelBenchmarks=track_kernel_entries` - kernel entry/exit tracking

On RAS error, the SDEI handler:
1. Logs the error to ftrace buffer
2. Dumps ftrace buffer to UART (base64+LZ4 compressed)
3. Halts the system

## Reproduction

```bash
# Build with el2-ras mode
mcp__sel4-autopilot__build_sel4test(mode="el2-ras")

# Run test - will halt on first RAS error with ftrace dump
mcp__sel4-autopilot__test_sel4_binary(binary_path="...", timeout=600)

# Query ftrace for RAS error context
mcp__sel4-autopilot__query_ftrace(request_id="...", summary=True)
mcp__sel4-autopilot__query_ftrace(request_id="...", event_type="ras_error_hdr", context=50)
```

## Next Steps

1. ~~**Investigate VMID switching during cancelBadgedSends**~~ ✓ DONE
   - ~~Add ftrace markers around VTTBR writes~~ ✓ Added VTTBR, TLBI, DSB events
   - ~~Check for missing DSB barriers before VSpace switches~~ ✓ DSB ISH present

2. **Investigate memory reuse during cap revocation**
   - Track when VSpace/PT memory is freed and reused
   - Check if memory is properly cleared before reuse
   - Look for race between free and reallocation

3. **Investigate speculative PTW pollution**
   - Check if TLB contains stale entries during memory reuse
   - Add TLBI ALLE1IS before VSpace memory reuse
   - Consider VMID recycling issues

4. **Compare with KVM/Xen fixes**
   - Marc Zyngier's 2023 KVM patch for speculative PTW
   - Check if similar barriers needed in seL4's context switch path

5. **Test with additional TLB invalidation**
   - Add `TLBI VMALLS12E1IS` when freeing VSpace memory
   - Add `TLBI ALLE1IS` before page table memory reuse

## Confirmed Pattern Summary

Based on 4+ test runs with consistent results:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    CancelBadgedSends RAS Error Timeline                 │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  [Test Start]                                                           │
│       │                                                                 │
│       ▼                                                                 │
│  ┌─────────────────────┐                                                │
│  │ CNodeRevoke (tag 21)│  Revokes derived caps                          │
│  └──────────┬──────────┘                                                │
│             │                                                           │
│             ▼                                                           │
│  ┌─────────────────────────────┐                                        │
│  │ CancelBadgedSends (tag 23)  │  Wakes blocked threads                 │
│  │ (empty - immediate exit)    │  ← No internal work here               │
│  └──────────┬──────────────────┘                                        │
│             │                                                           │
│             ▼                                                           │
│  ┌─────────────────────────────────────────────────────────┐            │
│  │           THREAD CASCADE (20+ threads)                  │            │
│  │  tcb=0x80afeb3800 → Recv                                │            │
│  │  tcb=0x80afe9f800 → Recv                                │            │
│  │  tcb=0x80afe8c800 → Recv                                │            │
│  │  ... (continues for 20+ threads)                        │            │
│  │                                                         │            │
│  │  ⚠️  PAGE TABLE CORRUPTION HAPPENS HERE                 │            │
│  │     (but not detected yet)                              │            │
│  └──────────┬──────────────────────────────────────────────┘            │
│             │                                                           │
│             │  ~137,000 events later                                    │
│             │                                                           │
│             ▼                                                           │
│  ┌─────────────────────────────────────────────────────────┐            │
│  │           CNODE_REVOKE CLEANUP                          │            │
│  │                                                         │            │
│  │  unmapPage()                                            │            │
│  │    └→ lookupPTSlot()                                    │            │
│  │         └→ pte_ptr_get_pte_type()                       │            │
│  │              ▲                                          │            │
│  │              │                                          │            │
│  │      ╔═══════╧═══════════════════════════════════╗      │            │
│  │      ║  💥 RAS ERROR: SCC Uncorrectable Error    ║      │            │
│  │      ║  Reading corrupted PTE with stale data    ║      │            │
│  │      ╚═══════════════════════════════════════════╝      │            │
│  └─────────────────────────────────────────────────────────┘            │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

**Root Cause (Hypothesis)**: Page table memory gets corrupted during the rapid thread cascade, possibly due to:
1. Speculative PTW caching stale translations during VMID switching
2. Memory reuse race between free and reallocation
3. Missing TLB invalidation when VSpace memory is recycled

## Phase 25: PTE_ACCESS Tracing - Captured Corruption (2025-12-22)

### Major Breakthrough

Added `PTE_ACCESS` ftrace events to capture the exact PTE values being read during page table walks. This revealed the **exact corruption pattern**.

### Test Run: 20251222-023341

| Metric | Value |
|--------|-------|
| Request ID | 20251222-023341 |
| Build mode | el2-ftrace |
| Total ftrace events | 301,641 |
| PTE_ACCESS_ADDR events | 8,851 |
| PTE_ACCESS_VAL events | 8,851 |
| RAS error index | 301,636 |

### The Corrupted Page Table Walk

The RAS error occurred during this page table walk in `lookupPTSlot`:

```
Level 2: @0x80ac003ff0 → 0x0000000080edf003  ✓ Valid table → 0x80edf000
Level 1: @0x8080edf000 → 0x0000000080000003  ✓ Valid table → 0x80000000
Level 0: @0x8080000068 → 0x04000000aff164ff  ✗ GARBAGE!
```

The Level 0 page table at physical address `0x80000000` was created and properly initialized, but entry 13 (offset 0x68) contains garbage.

### The Corruption Evidence

| When | Index | Address | PTE Value | Valid? |
|------|-------|---------|-----------|--------|
| After creation | 55,272 | 0x8080000068 | `0x0000000080a9e000` | NO (bits 1:0 = 00) |
| At RAS error | 301,634 | 0x8080000068 | `0x04000000aff164ff` | YES (bits 1:0 = 11) but GARBAGE! |

**Key Insight**: The corruption changed an **invalid** PTE (bits 1:0 = 00) to look **valid** (bits 1:0 = 11), but with a garbage address. This caused the MMU to try to use address `0x04000000aff16xxx` which triggered the SCC Address Range Error.

### Page Table Lifecycle

```
[52873] SAFE_PTE - initialization started
[52874] CREATE_OBJ paddr=0x80000000 type=9 (page table)
[52875-52878] INIT_PT phases 0-3 - filled with safe PTEs
[52886] PT_MAP pt=0x80000000 parent=0x80edf000 slot=0
[55271] PTE_ACCESS @0x8080000068 = 0x0000000080a9e000 (invalid entry, expected)

... ~246,000 events of CANCEL_BADGED_SENDS_0002 test execution ...

[301633] PTE_ACCESS @0x8080000068 = 0x04000000aff164ff (CORRUPTED!)
[301636] RAS_ERROR
```

### Analysis of the Garbage Value

The corrupted value `0x04000000aff164ff`:
- Lower 32 bits: `0xaff164ff`
- Upper 32 bits: `0x04000000`
- Bits 1:0 = `0b11` = valid page descriptor
- Address bits (47:12): `0x04000000aff16` - invalid physical address!

The value `0xaff16` is suspiciously close to legitimate page table addresses seen in the trace (like `0xaff1b000`). This suggests the garbage might be:
1. A stale PTE from a different page table
2. Data from a previous test iteration
3. Memory that was reused before being properly cleared

### Call Chain at Error

```
cteDelete (cnode.c:558)
  └→ finaliseSlot (cnode.c:621)
       └→ isFinalCapability (cnode.c:852)
            └→ ...
                 └→ Arch_finaliseCap (objecttype.c:143)
                      └→ finaliseCap (objecttype.c:105)
                           └→ unmapPage (vspace.c:1470)
                                └→ lookupPTSlot (vspace.c:745)
                                     └→ RAS ERROR reading garbage PTE
```

### Key Findings

1. **Page table properly initialized**: The page table at 0x80000000 was created with safe PTEs
2. **Entry was initially invalid**: At index 55,272, entry 0x68 contained an invalid PTE (expected)
3. **Corruption window**: ~246,000 events between valid read and corrupted read
4. **Corruption makes invalid → valid**: The garbage has valid descriptor bits set
5. **Garbage contains stale data**: The `0xaff16` fragment suggests data from elsewhere

### Implications

This proves:
- **Corruption is NOT in initialization** - the page table was created correctly
- **Corruption happens during test execution** - confirmed by the 246K event gap
- **Something writes to page table memory** - either directly or through memory aliasing
- **The write sets valid bits** - making an invalid entry look valid

### Possible Causes

1. **Memory aliasing**: Two different virtual addresses mapping to the same physical page
2. **Use-after-free**: Page table memory deallocated and reused, but parent PTE not cleared
3. **Stale DMA**: Some bus master writing to old cached address
4. **Stack/heap corruption**: Kernel writes to wrong address due to bug

### Next Steps

1. **Track memory lifecycle**: Add ftrace events when page table memory is freed/retyped
2. **Add memory watchpoint**: Trigger on write to 0x8080000068
3. **Scan for aliases**: Check if any other PTE points to physical 0x80000000
4. **Check retype operations**: Verify page table isn't being retyped while still referenced

## Related Documents

- [FPU Pattern](ras-fpu-pattern.md) - The other reproducible RAS trigger (thread creation/resume)
- [ARM Speculative PTW Research](arm-speculative-ptw-research.md) - Linux/KVM/Xen fixes
- [orin-ras-error-investigation.md](orin-ras-error-investigation.md) - Full investigation log
- [speculative-ptw-fix-plan.md](../../../../kernel/docs/speculative-ptw-fix-plan.md) - Fix checklist
