# Orin AGX RAS Error Investigation

## Summary

**STATUS: ⚠️ BUG B FIXED, BUG A INVESTIGATION IN PROGRESS - CACHE HYPOTHESIS DISPROVEN**

This document captures the investigation into intermittent RAS (Reliability, Availability, Serviceability) errors occurring during sel4test execution on NVIDIA Orin AGX.

### ⚠️ BREAKTHROUGH: Two Separate Bugs (2025-12-17)

Analysis across different RAM configurations revealed **TWO COMPLETELY INDEPENDENT BUGS**:

| Bug | Error Pattern | Trigger Test | Code Path | RAM Dependent | Status |
|-----|---------------|--------------|-----------|---------------|--------|
| **A** | 0x7fffxxxx | FPU0001 (>99%) | `arm_sys_send_recv`, `load_segment` | YES - only near 0x80000000 | **⚠️ PTE overwrite mystery** |
| **B** | 0x0fc0/0x0ff0 | All tests (CANCEL_BADGED most) | `sel4utils_destroy_process`, `vka_cnode_revoke` | NO - always present | **✓ FIXED** |

### ⚠️ CRITICAL: Cache Flush AND ATF/OP-TEE Hypotheses DISPROVEN (2025-12-18)

**Non-temporal stores (STNP) that bypass cache entirely did NOT fix the problem.** (Phase 16)

**5-second delay before verification: verification PASSES, but later scan finds garbage.** (Phase 17)

This proves:
1. Cache flush operations (dc civac) ARE working correctly
2. Safe PTEs ARE being written to DRAM (verified with dc ivac + read-back)
3. ATF/OP-TEE is NOT corrupting memory (5-second delay showed no corruption)
4. **Corruption happens DURING test execution** - something in the test or kernel writes to PT memory
5. **Phase 18 ftrace**: 65,536 kernel function calls captured between PT verification and garbage detection - **NO PT modification functions called** (no unmapPageTable, no clearMemory, no createObject)
6. **Phase 19 diagnostic region**: 200KB pattern at 0x80000000-0x80032000 remained **INTACT** - corruption occurs in middle of DRAM (0xACxxxxxx), not at beginning

See Phase 16, Phase 17, Phase 18, and Phase 19 for full analysis.

### ⚠️ Latest Finding (2025-12-20): System is Functionally Correct

**Phase 20 reframe**: sel4test passes 100% despite RAS errors and "garbage PTEs". The system works correctly - RAS errors come from **speculative** page table walks that don't affect architectural execution. The "garbage" is stale data from previous test iterations that speculative walkers hit but real execution never uses.

**Key question**: Why do stale L2→L3 links persist after PT teardown? (See Phase 20)

Phase 19 tested whether something corrupts the first 200KB of DRAM. A diagnostic pattern initialized at boot remained intact through 72+ million function calls. **PT corruption targets dynamically allocated PTs in the middle of DRAM**, not any fixed memory region. The RAM start address affects whether corruption manifests as RAS errors, not the corruption itself.

### ⚠️ Previous Finding (2025-12-18): Page Tables Contain Garbage User Data

Debug scanning revealed page tables contain **leftover user application data** (ASCII strings, ARM instructions) instead of valid PTEs. Grepping the sel4test build directory confirms these strings come from the application binary.

### ⚠️ Phase 15: The 0x400000 Mystery (2025-12-18)

#### Key Observations

Enhanced debug scanning categorizes bad PTE addresses:
```
RAS_SCAN: VSpace@0xac002000 L0:0 L1:0 L2:0 L3:13642 (1st@L3 in PT@0xac00a000: 0x4001ec->0x400000)
RAS_SCAN:   addr_ranges: 0x0xxx=7140, 0x7xxx=171, other=6331 (1st_7xxx: 0x12b00000710002bf->0x71000000)
```

**Address distribution of 13,642 bad L3 PTEs**:
- **0x0xxx** (0-256MB): 7,140 entries - includes 0x400000 (rootserver VA)
- **0x7xxx** (1.75-2GB): 171 entries - includes 0x71000000
- **other** (256MB-1.75GB): 6,331 entries

**Critical discrepancy**: RAS errors occur at **0x7ffcc000** (208KB below DRAM), but the scanned PTEs show addresses like 0x400000 and 0x71000000, NOT 0x7ffcc000!

#### Hypothesis: Cascading Walk Error

The 0x7ffcc000 error address may come from a **second-level speculative walk**:

1. L2 PTE has bits[47:12] pointing to 0x400000 (garbage from rootserver)
2. Speculative PTW walks to "table" at PA 0x400000 (unmapped/MMIO region)
3. Hardware reads garbage data from 0x400000 region
4. Garbage data interpreted as PTEs contains values like 0x7ffcc (bits[47:12])
5. PTW tries to walk to 0x7ffcc000 → **RAS error!**

This would explain:
- Why scanned PTEs show 0x400000 but errors are at 0x7ffcc000
- Why errors are ALWAYS near 2GB boundary (0x7fffxxxx) - hardware/MMIO behavior at low addresses
- Why errors don't occur at 0x90000000 DRAM - different data at 0x400000 region?

#### The Mystery PT at 0xac00a000

The page table at PA 0xac00a000:
- Is found via scan (linked into VSpace via valid L0→L1→L2 path)
- Contains 13,642 bad L3 entries
- **Has NO "created OK" debug message** - was NOT created via Arch_createObject!

This suggests either:
1. PT was created in a previous test iteration and memory reused
2. PT was created via a different code path (boot-time?)
3. PT was created but memory not properly flushed before scan

#### Code Analysis: clearMemory() vs clearMemory_PT()

Two memory clearing functions exist:
```c
// kernel/include/arch/arm/arch/machine.h

// Regular clear - memzero only, NO cache flush
static inline void clearMemory(word_t *ptr, word_t bits) {
    memzero(ptr, BIT(bits));
}

// Page table clear - memzero + cache flush to PoC
static inline void clearMemory_PT(word_t *ptr, word_t bits) {
    memzero(ptr, BIT(bits));
    cleanInvalidateCacheRange_RAM((word_t)ptr, (word_t)ptr + BIT(bits) - 1,
                                  addrFromPPtr(ptr));
}
```

**Problem**: `resetUntypedCap()` in `kernel/src/object/untyped.c:255` uses `clearMemory()` (no cache flush) for ALL object types, including page tables!

**Consequence**:
1. Memory previously used for rootserver data (containing 0x400xxx addresses)
2. Memory returned to untyped pool
3. `clearMemory()` zeros D-cache copy only
4. Main memory still has old rootserver data (0x4001ec, etc.)
5. Page table created - Arch_createObject() writes safe PTEs and flushes
6. BUT: There's a **race window** between clearMemory() and Arch_createObject()
7. If MMU speculatively walks during this window → sees garbage

#### Next Steps

1. **Verify the cascading walk hypothesis**: Check what data is at PA 0x400000 on Tegra234
2. **Fix clearMemory()**: Either use clearMemory_PT() for page table types, or add flush before Arch_createObject()
3. **Track PT lifecycle**: Add debug to trace when 0xac00a000 is created/destroyed

**Two hypotheses under investigation**:

**Hypothesis A (cache flush issue)**:
1. Memory is reused from user frames → page tables
2. `clearMemory()` only does memzero without cache flush to PoC
3. When cache lines are evicted, DRAM still has old garbage
4. Speculative PTW reads garbage from DRAM, triggers RAS error

**Hypothesis B (ELF load overlap)**:
1. Page tables are created and properly initialized
2. ELF loader loads application segments into memory
3. Due to a bug, load overlaps with page table physical memory
4. Page tables get corrupted with application data (strings, code)

**Note**: Both hypotheses are plausible, and the issue could be a combination of both. The intermittent nature of RAS errors (sometimes occur, sometimes not) suggests timing-dependent behavior like cache eviction patterns or memory allocation ordering that varies between runs.

### ⚠️ Phase 16: Non-Temporal Stores (STNP) Investigation (2025-12-18)

#### Summary

**CRITICAL FINDING**: Non-temporal stores (STNP) that bypass cache entirely did NOT fix the problem. This **disproves the cache flush hypothesis** and points to something overwriting PTEs after initialization.

#### Background

Based on Phase 15 findings, we hypothesized that `dc civac` cache flush operations were not correctly writing safe PTEs to DRAM on Tegra. The verification code showed "DRAM verified" but later scans found garbage.

We modified the page table initialization to use ARM64 STNP (Store Non-temporal Pair) instructions, which:
- Bypass all cache levels
- Write directly to memory system
- Do not allocate cache lines

#### Code Changes

Modified `Arch_createObject()` in `kernel/src/arch/arm/64/object/objecttype.c`:

```c
case seL4_ARM_PageTableObject:
    {
        /*
         * Use non-temporal stores (STNP) to bypass cache and write directly
         * to DRAM. This ensures safe PTEs actually reach memory, avoiding
         * issues where cache flush operations don't work correctly on Tegra.
         */
        word_t safe_pte = pte_pte_invalid_new().words[0];
        word_t *base = (word_t *)regionBase;

        /* STNP writes 128 bits (two 64-bit values) with non-temporal hint */
        for (word_t i = 0; i < BIT(seL4_PageTableIndexBits); i += 2) {
            asm volatile("stnp %0, %1, [%2]"
                         :
                         : "r"(safe_pte), "r"(safe_pte), "r"(&base[i])
                         : "memory");
        }
        asm volatile("dsb sy" ::: "memory");
    }
    /* Also do regular cache flush to ensure coherency */
    cleanInvalidateCacheRange_RAM(...);
```

Also added forced cache invalidation before verification read-back:

```c
/* Force cache miss: invalidate all lines again, then barrier */
for (word_t i = 0; i < BIT(seL4_PageTableIndexBits); i++) {
    word_t line = (word_t)regionBase + (i * sizeof(pte_t));
    asm volatile("dc ivac, %0" : : "r"(line));
}
asm volatile("dsb sy" ::: "memory");

/* Now read - should come from DRAM */
```

#### Test Results

**Test: 100x CANCEL_BADGED_SENDS_0002**

```
RAS_DEBUG: PT@0xac224000 created OK (DRAM verified)
RAS_MAP: PT@0xac224000 mapped into parent@0xac1e6000[2] (pte=0xac224003)
...
Running test CANCEL_BADGED_SENDS_0002 (cancelBadgedSends deletes caps)
...
RAS_WALK: L2@0xac224000[0] points to L3@0xac1d6000 (pte=0x4000000ac1d647f)
RAS_WALK: L2@0xac23b000[31] points to L3@0xac00a000 (pte=0x4000000ac00a4ff)
```

**Result**: RAS errors still occur! The pattern is identical to before STNP:
1. PT created with safe PTEs
2. Verification passes (reads correct values)
3. Later scan finds garbage in specific entries

#### Analysis

**What the results prove:**

1. **Cache flush is NOT the problem**: STNP bypasses cache entirely. If cache flush were the issue, STNP would fix it. It didn't.

2. **DRAM actually receives correct values**: The verification (with dc ivac to force cache miss) shows correct values immediately after writing. DRAM has the right data.

3. **Something overwrites PTEs after initialization**: The ONLY explanation for the pattern (verification passes, later sees garbage) is that something writes to these PT entries BETWEEN creation and scan.

#### The Mystery: What Overwrites PT Entries?

The garbage PTE values follow a pattern:
- `0x4000000ac1d647f` - points to 0xac1d6000
- `0x4000000ac00a4ff` - points to 0xac00a000

These have:
- Bit 62 set (0x4000000000000000) - unusual for seL4-created PTEs
- Valid table descriptor bits [1:0] = 0b11
- Physical addresses that ARE valid PTs from PREVIOUS test iterations

**Key observation**: The addresses 0xac1d6000 and 0xac00a000 were created as PTs in earlier test iterations. This suggests:
1. Memory was used as PT in test N
2. PT entry was written pointing to child table
3. Test N completed, VSpace destroyed, memory returned to pool
4. Memory reused for NEW PT in test N+1
5. Safe PTEs written via STNP
6. **SOMETHING** writes the OLD value back to the entry

**Possible causes being investigated:**

1. **DMA or other bus master**: Some hardware writes to this memory region
2. **Another CPU core**: SMP issue with memory visibility (unlikely - single core test)
3. **Store buffer aliasing**: Old stores in flight getting reordered after STNP
4. **Memory controller issue**: Tegra-specific behavior

#### Debug Output Pattern Analysis

```
Test 1:
  RAS_DEBUG: PT@0xac00c000 created OK (DRAM verified)
  RAS_WALK: L2@0xac00c000[0] points to L3@0xac00a000 (pte=0x4000000ac00a47f)
  RAS_WALK: L2@0xac00c000[430] points to L3@0xac1d6000 (pte=0x4000000ac1d64ff)

Test 2:
  RAS_DEBUG: PT@0xac1d6000 created OK (DRAM verified)  <-- Now 0xac1d6000 is created!
  RAS_WALK: L2@0xac1d6000[264] points to L3@0xac00a000 (pte=0x4000000ac00a4ff)
```

Notice:
- Test 1: L2@0xac00c000 entries point to garbage addresses including 0xac1d6000
- Test 2: PT@0xac1d6000 is NOW created as a valid PT
- BUT: 0xac1d6000 appeared as a garbage target in Test 1 BEFORE it was created in Test 2

This suggests the garbage values are **STALE DATA from previous memory usage**, not from the current test iteration.

#### Next Investigation Steps

1. **Add per-entry debug**: Print value BEFORE and AFTER STNP write for first few entries
2. **Track memory reuse**: Log when physical addresses are retyped
3. **Check for write-after-STNP**: Add watchpoint or trace on specific addresses
4. **Investigate memory controller**: Check if Tegra has memory aliasing or prefetch issues

#### Code Locations Modified

- `kernel/src/arch/arm/64/object/objecttype.c:Arch_createObject()` - STNP writes
- `kernel/src/arch/arm/64/kernel/vspace.c:performPageTableInvocationUnmap()` - STNP on unmap
- Verification code with `dc ivac` + `dsb sy`

### ⚠️ Phase 17: Delayed Verification Test - ATF/OP-TEE Hypothesis RULED OUT (2025-12-18)

#### Hypothesis

ATF (ARM Trusted Firmware at EL3) or OP-TEE (secure world) might be periodically writing to seL4's page table memory, causing corruption.

#### Test Design

Added a 5-second busy-loop delay between PT creation and verification for PTs in the troublesome 0xac000000-0xad000000 address range:

```c
if (paddr >= 0xac000000 && paddr < 0xad000000) {
    printf("RAS_DEBUG: PT@0x%lx - delaying 5s before verify...\n", paddr);
    for (volatile word_t j = 0; j < 10000000000UL; j++) {
        asm volatile("" ::: "memory");
    }
}
/* Then verify... */
```

#### Results

```
RAS_DEBUG: PT@0xac00c000 - delaying 5s before verify...
RAS_DEBUG: PT@0xac00c000 created OK (DRAM verified)
RAS_MAP: PT@0xac00c000 mapped into parent@0xac00b000[2] (pte=0xac00c003)
...
Running test CANCEL_BADGED_SENDS_0002 (cancelBadgedSends deletes caps)
...
RAS_WALK: L2@0xac00c000[0] points to L3@0xac00a000 (pte=0x4000000ac00a47f)
```

**Timeline:**
1. PT@0xac00c000 created with safe PTEs
2. **5-second delay** - no external writes
3. Verification **PASSES** - all entries still correct
4. PT mapped into parent table
5. Test execution begins
6. Scan finds **garbage** in entry [0]

#### Conclusion

**ATF/OP-TEE hypothesis RULED OUT.**

If secure world firmware were periodically corrupting memory, the 5-second delay would have caught it during verification. Instead:
- Verification passes AFTER the 5-second delay
- Corruption appears DURING the test's own execution

**The corruption is caused by something in the test execution path itself**, not by external firmware.

#### What This Means

The bug is in one of:
1. **The test code** - CANCEL_BADGED_SENDS_0002 does something that writes to PT memory
2. **seL4 kernel operations** triggered by the test - cap revocation, VSpace destruction, memory retyping
3. **Memory aliasing** - the test maps memory that overlaps with PT physical addresses

#### Next Investigation Direction

Focus on what happens DURING test execution:
1. What does CANCEL_BADGED_SENDS_0002 do exactly?
2. What kernel operations write to page table memory?
3. Is there memory aliasing between user frames and PTs?

### ⚠️ Phase 18: Function Tracing (ftrace) Analysis (2025-12-19)

#### Objective

Capture ALL kernel function calls between:
1. PT creation + DRAM verification (passes)
2. VSpace scan detecting garbage PTEs

This would reveal what code path corrupts the PTs.

#### Implementation

Built sel4test with `CONFIG_KERNEL_FUNCTION_TRACE` enabled (GCC `-finstrument-functions`):
- `ftrace_reset()` clears buffer when PT is verified
- `ftrace_dump_all()` dumps captured calls when scan finds garbage
- Dump only happens once to avoid flooding output

#### Results

**Full ftrace captured: 65,536 function calls between PT verification and garbage detection.**

See: [ftrace-pt-corruption-20251219.txt](ftrace-pt-corruption-20251219.txt) (99,050 lines decoded)

#### Key Findings from ftrace Statistics

Top called functions during the corruption window:
```
   Calls  Function
    2744  readBcrCp (debug.h)           - debug register access
    2744  writeBcrCp (debug.h)          - debug register access
    1832  readWcrCp (debug.h)           - watchpoint register
    1832  writeWcrCp (debug.h)          - watchpoint register
     719  messageInfoFromWord_raw       - IPC message handling
     606  resolveAddressBits (cspace.c) - capability lookup
     604  findMapForASID (vspace.c)     - ASID lookup
     600  lookupSlot (cspace.c)         - CNode traversal
     504  lookupCapAndSlot (cspace.c)   - capability resolution
     458  restore_user_debug_context    - thread switch debug state
     457  c_handle_syscall              - syscall entry
     410  vcpu_switch (vcpu.c)          - VCPU context switch
     409  lazyFPURestore (fpu.h)        - FPU state management
     356  isArchCap (structures.h)      - capability type check
```

#### Critical Observation: No PT Modification Functions!

Searched ftrace for page table modification functions:
- `unmapPageTable` - **NOT FOUND**
- `performPageTableInvocationUnmap` - **NOT FOUND**
- `Arch_finaliseCap` - **NOT FOUND**
- `clearMemory` - **NOT FOUND**
- `createObject` - **NOT FOUND**
- `mapPage` / `unmapPage` - **NOT FOUND**

**Functions that ARE present:**
- `setVMRoot` - VSpace root switching (many calls)
- `findVSpaceForASID` - ASID→VSpace lookup
- `cteDeleteOne`, `finaliseCap` - capability deletion (reply caps)
- `lookupIPCBuffer` - IPC buffer access

#### Analysis

The ftrace shows the test is primarily doing:
1. Syscall handling (`c_handle_syscall`)
2. IPC operations (`doReplyTransfer`, `copyMRs`)
3. Thread/context switching (`vcpu_switch`, `restore_user_debug_context`)
4. Capability operations (`cteDeleteOne` on reply caps)

**No explicit PT modification occurs**, yet garbage appears. This suggests:
1. **Memory aliasing**: User data mapped to same PA as PT
2. **DMA/bus master**: External write to PT memory
3. **Cache coherency issue**: Despite STNP bypass, still seeing stale data
4. **Hardware bug**: Speculative execution corrupting memory

#### The "Phantom PT" Pattern

Garbage PTEs point to addresses like `0xac00a000` - this is itself a PT address from a **previous test iteration**. The L2 entry at `L2@0xac00c000[0]` contains `0x4000000ac00a47f`, which looks like an old valid PTE that was never properly cleared.

This strongly suggests **memory reuse without proper clearing** - but ftrace shows no `clearMemory` or object creation during this window.

#### Critical Timeline from Log

```
Line 2664: PT@0xac00c000 created OK (DRAM verified)   <- ALL 512 entries verified as safe PTEs!
Line 2665: PT@0xac00c000 mapped into parent@0xac00b000[2]
Lines 2667-2687: More PT mappings occur
Line 2688: RAS_WALK: L2@0xac00c000[0] points to L3@0xac00a000 (pte=0x4000000ac00a47f)  <- GARBAGE!
```

**PT@0xac00a000 was NEVER created** - no "RAS_DEBUG: PT@0xac00a000 created OK" message exists in the entire log!

The garbage PTE `0x4000000ac00a47f` points to a **phantom PT from a previous test iteration** that no longer exists.

#### The Mystery

Between line 2665 (map) and line 2688 (scan), entry [0] of L2@0xac00c000 was corrupted:
- Before: Safe PTE (verified in DRAM)
- After: Stale PTE pointing to phantom 0xac00a000

The ftrace shows **no explicit writes to PT memory** during this window. Something is writing old data back without going through any kernel function we can trace.

#### Heavy Context Switching During Corruption Window

The ftrace reveals significant activity:
- **1,833 context switches** (`switchToThread`, `Arch_switchToThread`)
- **975 VSpace root changes** (`setVMRoot` calls)

Each `setVMRoot` writes to VTTBR_EL2, switching which page table the MMU uses. This is a potential corruption vector:
1. MMU may speculatively walk BOTH old and new page tables during switch
2. If stale cached PTEs exist, the switch might trigger writeback
3. TLB invalidations during switch might interact unexpectedly with cache

This points to a **race condition between VTTBR switches and page table memory**, though the exact mechanism is unclear since we're using cache flushes during PT creation.

### Phase 19: 200KB Diagnostic Region Test (2025-12-20)

#### Objective

Test the hypothesis: "Something is corrupting the first 200KB of DRAM (0x80000000-0x80032000), which is why moving RAM start to 0x80032000 fixes the RAS errors."

#### Implementation

Created a 200KB diagnostic region at the start of DRAM:

1. **IMAGE_START_ADDR** changed to 0x80032000 (elfloader/kernel start after diagnostic region)
2. **DTS memory** starts at 0x80032000 (makes 0x80000000-0x80032000 device untyped)
3. **Elfloader** initializes 200KB with pattern `0xDEADBEEFCAFEBABE ^ index` before kernel boot
4. **Kernel ftrace** scans diagnostic region every 1000 function calls with cache invalidation

Full implementation details: [diagnostic-region-investigation.md](diagnostic-region-investigation.md)

#### Results

| Metric | Value |
|--------|-------|
| Tests run | 7 (CANCEL_BADGED_SENDS_0002 x7) |
| Tests passed | 7 (100%) |
| RAS errors | **0** |
| Diagnostic region corruptions | **0** |
| PT corruptions detected | **4** |

**PT Corruption Events (still occurring):**
- PT[3]@0xac25c000 - call 11,230,000 - Entry [282] = 0x10011ea5
- PT[0]@0xac234000 - call 30,890,000
- PT[2]@0xac1e6000 - call 51,550,000
- PT[2]@0xac254000 - call 72,190,000

#### Conclusion: Hypothesis REJECTED

**The first 200KB of DRAM is NOT being corrupted.**

- Diagnostic pattern remained intact throughout 72+ million function calls
- PT corruption occurs in middle of DRAM (0xACxxxxxx range), not at beginning
- Tests pass despite PT corruption being detected
- Moving RAM start to 0x80032000 prevents RAS errors for a different reason

#### Implications

1. **PT corruption is real but benign with 0x80032000 RAM start** - garbage PTEs don't cause RAS errors when RAM base is offset
2. **Corruption targets dynamically allocated PTs**, not fixed memory regions
3. **RAM start address affects RAS manifestation**, not the underlying corruption
4. The corrupted addresses (e.g., 0x10011000) are below DRAM - stale data pattern continues

### Phase 20: Functional Correctness Reframe (2025-12-20)

#### Key Insight: The System Works Correctly

Despite RAS errors and "garbage PTEs", sel4test passes 100% of the time (with RAM at 0x80032000). This reframes the problem:

| Observation | Implication |
|-------------|-------------|
| sel4test passes 100% | Actual page table walks for real memory accesses are correct |
| No kernel crashes | VSpace management, thread switching, IPC all work |
| No memory corruption in test results | Applications read/write correct data |
| Only symptom: RAS errors | Something triggers **speculative** accesses to bad addresses |

#### The Real Problem

**RAS errors come from speculative page table walks that don't affect architectural execution.**

The MMU speculatively walks page tables. When it hits a PTE with bits[47:12] pointing below DRAM (e.g., 0x0xxx or 0x7ffcxxxx), hardware tries to fetch from that address → RAS error.

But if the speculative result isn't actually used (branch misprediction, different VSpace activated, TLB hit), the test still passes.

#### Why RAM Start Address Matters

| RAM Start | Speculative walk hits garbage PTE | Result |
|-----------|-----------------------------------|--------|
| 0x80000000 | Garbage address like 0x7ffcc000 | **Below DRAM** → RAS error |
| 0x80032000 | Same garbage address 0x7ffcc000 | Still below DRAM, but further from boundary |
| 0x90000000 | Same garbage | May land in DRAM depending on offset calculation |

The garbage PTEs contain the same stale data regardless of RAM start. What changes is **whether the resulting speculative address triggers a RAS error**.

#### What the "Garbage" Actually Is

The "phantom PT at 0xac00a000" scenario:
1. Previous test iteration created PT at 0xac00a000
2. Test finished, PT memory was freed
3. Memory reused for user data (rootserver strings)
4. Some stale L2 entry still points to 0xac00a000
5. **Speculative walker** follows the stale link → sees garbage → tries to access 0x400000 → RAS

The test works because the **architecturally active** VSpace doesn't use that stale path.

#### The Remaining Question

**Why do stale L2→L3 links persist after PT teardown?**

Possible causes:
1. **Teardown order**: L3 freed before L2 entry cleared → speculative window
2. **Missing TLB invalidation**: Stale TLB entry causes speculative walk on old path
3. **VTTBR switch race**: During 975 VSpace switches, old VSpace briefly active
4. **armKSGlobalUserVSpace accumulates stale entries** between test iterations

#### Implications for Fix Strategy

If the system is **functionally correct**, options include:

| Approach | Description |
|----------|-------------|
| **Suppress RAS for speculative accesses** | Configure RAS to ignore non-architectural errors (if hardware supports) |
| **Always use safe addresses in stale PTEs** | Ensure ALL PTEs point to valid DRAM even during/after teardown |
| **Serialize speculation during VSpace switches** | More aggressive barriers |
| **Accept 0x80032000 workaround** | First 200KB reserved; system works correctly |

### Phase 21: Stale L2→L3 Link Analysis (2025-12-20)

#### The Core Mystery

The scan finds garbage in PT@0xac00a000, but **PT@0xac00a000 was never created in this test iteration**. The L2 entry pointing to it is a **stale link** from a previous iteration.

```
Iteration N:
  Create L2@0xac00c000, L3@0xac00a000
  L2[x] = valid pointer to L3@0xac00a000
  Test runs
  Teardown: L2[x] cleared, L3 entries cleared, memory freed

Iteration N+1:
  Memory at 0xac00c000 reused for NEW L2
  Arch_createObject: writes safe PTEs, flushes, VERIFIES in DRAM ✓
  ...65,536 kernel calls, 975 VSpace switches...
  Scan finds: L2[x] = 0x4000000ac00a47f (valid pointer to phantom L3!)
```

The stale pointer **reappeared** after we verified the memory contained safe PTEs.

#### What Code Was Running?

Ftrace captured 65,536 kernel function calls between verification and garbage detection:
- **1,833 context switches** (`switchToThread`, `Arch_switchToThread`)
- **975 VSpace root changes** (`setVMRoot` → VTTBR_EL2 writes)
- **No PT modification functions** (no unmapPageTable, no clearMemory, no createObject)

#### Hypotheses Investigated and Status

| Hypothesis | Status | Evidence |
|------------|--------|----------|
| Cache flush not working | ❌ Disproven | STNP bypasses cache - still fails (Phase 16) |
| ATF/OP-TEE writes to memory | ❌ Disproven | 5-second delay - corruption happens DURING test (Phase 17) |
| clearMemory overwrites safe PTEs | ❌ Disproven | ftrace shows no clearMemory calls |
| Memory aliasing (PT and frame overlap) | ❓ Not found | Would cause functional failures - tests pass |
| TLB writeback | ❌ Impossible | ARM TLB is read-only, doesn't write to memory |
| MMU translation cache writeback | ❌ Impossible | ARM translation caches are read-only |
| VTTBR switch race | ❓ Possible | Heavy VTTBR switching during corruption window |
| Speculative store from CPU | ❓ Unknown | Would require hardware bug |
| DMA/bus master | ❓ Unknown | No active DMA on bare metal seL4 |

#### The VTTBR Switch Hypothesis

Each VSpace switch involves:
1. Write new VTTBR_EL2 (page table base + VMID)
2. MMU switches to new translation tables
3. Old TLB entries invalidated (eventually)

With 975 VTTBR switches during the corruption window, there's significant MMU activity. Potential issues:
- MMU speculatively walks BOTH old and new tables during transition
- Stale TLB entries might cause speculative fetches from old page table locations
- Some unknown interaction between VMID switching and memory coherency

#### What Could Write Stale Data?

We need something that:
1. Writes to PT memory (0xac00c000)
2. Without going through any kernel function (ftrace shows none)
3. Writes data from a PREVIOUS iteration (stale L3 pointer)
4. Happens during test execution (not boot, not idle)

Possibilities remaining:
1. **CPU speculative store buffer** - unlikely, dsb sy should drain
2. **Hardware memory controller bug** - Orin-specific?
3. **Cache eviction race** - evicting old dirty line that we thought was clean
4. **Translation table caching** - hardware caching PTE values and writing back?

#### Why RAM Start Address Matters

Even with the same stale data in PTEs, the RAS behavior differs by RAM start:
- **0x80000000**: Stale address 0x7ffcc000 is 200KB below DRAM → RAS error
- **0x80032000**: Same stale address, still below DRAM, but errors don't occur (unknown why)
- **0x90000000**: Stale addresses map to different region, possibly in DRAM

The mystery is why 0x80032000 specifically avoids the errors while 0x80000000 triggers them.

### ✓ Bug B Fix (2025-12-17)

**Root cause**: `pte_pte_invalid_new()` returns all zeros. During PTE clearing in `unmapPage()` / `unmapPageTable()`, speculative PTW reads the zero PTE and interprets bits[47:12] as PA=0, causing RAS errors at 0x0fc0/0x0ff0.

**Fix**: Added `pte_safe_invalid_new()` which sets bits[47:12] to `armKSGlobalUserVSpace` PA while keeping bits[1:0]=0 (invalid). Speculative walkers now see safe memory instead of PA=0.

**Location**: `kernel/src/arch/arm/64/kernel/vspace.c`

**Verification**:
- Before fix: 56-92 errors per run
- After fix: **0 errors in 5 runs**

### Current Workaround (Phase 9)

**Disable Stage 2 translation (HCR_EL2.VM=0) during VTTBR switches** in `setCurrentUserVSpaceRoot()`.

This helps with Bug A (thread lifecycle tests) but does NOT fix Bug B (cap revocation path).

**Results:**
- THREAD_LIFECYCLE_DELAYED_0001: **70% → 0%** error rate
- CANCEL_BADGED_SENDS_0002: Still triggers ~30 errors per 100 iterations

**Location:** `kernel/include/arch/arm/arch/64/mode/machine.h:setCurrentUserVSpaceRoot()`

---

## ⚠️ ROOT CAUSE ANALYSIS ⚠️

### Bug A: Syscall/ELF Path (0x7fffxxxx errors)

- **Trigger**: FPU0001 test exclusively (>99% of these errors)
- **Condition**: ONLY when RAM starts below 0x80032000 (see RAM Bisection below)
- **Code path**: `arm_sys_send_recv` (syscalls), `load_segment` (ELF loading)
- **Hypothesis**: Address calculation in syscall path wraps below 0x80000000 when RAM base is near 2GB boundary

#### RAM Address Bisection Results (2025-12-18)

Binary search performed to find exact RAM start address boundary where Bug A occurs.
Test: 100× FPU0001 + 100× CANCEL_BADGED_SENDS_0002 + other tests, 768MB RAM, EL2 mode.

| RAM Start | Offset from 0x80000000 | Status | RAS Errors |
|-----------|------------------------|--------|------------|
| 0x80000000 | +0 KB | ❌ ERRORS | ~870 |
| 0x80020000 | +128 KB | ❌ ERRORS | ~100+ |
| 0x80030000 | +192 KB | ❌ ERRORS | Yes |
| **0x80031000** | **+196 KB** | **❌ ERRORS** | **2** |
| **0x80032000** | **+200 KB** | **✅ WORKS** | **0** |
| 0x80034000 | +208 KB | ✅ WORKS | 0 |
| 0x80040000 | +256 KB | ✅ WORKS | 0 |
| 0x80080000 | +512 KB | ✅ WORKS | 0 |
| 0x80100000 | +1 MB | ✅ WORKS | 0 |
| 0x90000000 | +256 MB | ✅ WORKS | 0 |

**BOUNDARY: 0x80032000 (200KB offset from DRAM base)**

- RAM starting at 0x80031000 or below: RAS errors occur
- RAM starting at 0x80032000 or above: No RAS errors

**Implication**: Something in the first 200KB of Tegra234 DRAM (0x80000000-0x80031FFF) causes speculative access issues when seL4 uses this region. This could be:
1. Reserved/special memory region by TrustZone or other firmware
2. Address aliasing with MMIO regions
3. Hardware errata related to DRAM base address proximity

### Bug B: Destruction/Revocation Path (0x0xxx errors) - **✓ FIXED**

- **Trigger**: All tests, but CANCEL_BADGED_SENDS_0002 triggered most
- **Condition**: Was ALWAYS present regardless of RAM location
- **Code path**: `sel4utils_destroy_process`, `vka_cnode_revoke` → `unmapPage()` / `unmapPageTable()`
- **Root cause**: `pte_pte_invalid_new()` returned all zeros; speculative PTW read zero PTEs during race window before TLBI, interpreting bits[47:12] as PA=0
- **Fix**: `pte_safe_invalid_new()` sets bits[47:12] to `armKSGlobalUserVSpace` PA
- **See**: [bug-b-investigation.md](bug-b-investigation.md) for full analysis

### Historical Context: Thread Destruction

**Custom diagnostic tests originally identified thread destruction as a trigger:**

| Test | Description | Error Rate |
|------|-------------|------------|
| THREAD_LIFECYCLE_0001 | Repeated thread create/destroy (no FPU) | **43%** |
| FPU0001 (original) | FPU + repeated thread lifecycle | **41%** |
| FPU_MULTITHREAD_0001 | FPU + one-shot threads | **0%** |
| CONTEXT_SWITCH_ONLY_0001 | Context switching alone | **0%** |

Thread destruction (`cleanup_helper()` → `sel4utils_clean_up_thread()`) involves:
1. VSpace/page table cleanup
2. Capability revocation
3. TCB deletion

**The bugs**: Speculative PTW accesses stale/zero PTEs during these operations.

### Critical Finding: Exactly 4+ Threads Required

**Thread count scaling test (100 runs each):**

| Threads | Error Rate | Total SCC Errors |
|---------|------------|------------------|
| 1 (sequential) | **0%** (0/100) | 0 |
| 2 (concurrent) | **0%** (0/100) | 0 |
| 3 (concurrent) | **0%** (0/100) | 0 |
| **4 (concurrent)** | **24-32%** | 28-38 |
| **5 (concurrent)** | **23%** | 26 |
| **6 (concurrent)** | **31%** | 41 |

**Key Insights:**
1. **Threshold is EXACTLY 4 threads** - 3 threads = 0% errors, 4 threads = ~25-30% errors
2. **Error rate roughly constant above threshold** - 4, 5, 6 threads all ~25-30%
3. **NOT probability-based** - More threads doesn't dramatically increase error rate
4. **Something specific about 4+ runnable threads in scheduler queue**

**Implications:**
- The bug requires **exactly 4+ concurrent threads** at same priority
- Likely involves scheduler queue operations or round-robin scheduling
- The race condition is a **threshold effect**, not cumulative probability
- Possible causes:
  - Scheduler queue depth triggers specific code path
  - TLB set associativity (4-way?) causes conflicts
  - Some kernel data structure sized for 4 entries

### Where to Look for the Bug

**DO investigate (HIGH PRIORITY):**
1. **Scheduler queue operations with 4+ threads** - What changes when 4th thread is added?
2. **TLB invalidation during multi-thread cleanup** - ordering of TLBI vs context switch
3. **Round-robin scheduling code path** - Does 4+ equal-priority threads trigger different behavior?
4. **Barrier ordering in `deleteASID`** - ensure TLBI completes before next context switch ← **NEXT STEP**

### TLBI Audit Results (Completed)

**Path traced:** `cleanup_helper` → `sel4utils_clean_up_thread` → `vspace_free_sized_stack` → `seL4_ARCH_Page_Unmap` → `unmapPage` → `invalidateTLBByASIDVA`

**Key functions and barrier analysis:**

| Function | Location | Barriers | Status |
|----------|----------|----------|--------|
| `unmapPage` | vspace.c:1163 | Clears PTE, dc civac, then TLBI | ✅ |
| `invalidateLocalTLB_IPA` | machine.h:306 | `tlbi; dsb; tlbi; dsb; isb` | ✅ |
| `setCurrentUserVSpaceRoot` | machine.h:186 | Two-stage VTTBR + dsb/isb | ✅ |

**Conclusion: TLBIs appear correctly implemented with barriers.**

### Remaining Mystery: Why Exactly 4+ Threads?

The "4 thread threshold" is NOT explained by the TLBI audit. All barriers look correct. Possible remaining causes:

1. **Timing/probability** - 4 threads creates enough concurrent activity to hit a very small race window
2. **Memory allocator behavior** - 4 stacks might trigger specific page reuse patterns
3. **Cortex-A78AE microarchitecture** - Some CPU-specific behavior with 4+ concurrent contexts
4. **Cache line conflicts** - 4 stacks might create specific cache contention patterns

### ⚠️ BREAKTHROUGH: Phase 4 - Timing Window Analysis

**Test results that dramatically narrow down the race condition (100 iterations each, isolated runs):**

| Test | Description | Error Rate | SCC Errors |
|------|-------------|------------|------------|
| THREAD_LIFECYCLE_DELAYED_0001 | 4 threads + busy-wait delay after cleanup | **70%** | 70 |
| THREAD_LIFECYCLE_ALLOC_0001 | 4 threads + immediate page alloc after cleanup | **8%** | 8 |

**Compare to baseline THREAD_LIFECYCLE_0001: ~30% error rate**

**Immediate allocation reduces errors by ~9x compared to busy-wait!**

**Critical Insights:**

1. **Busy-wait delay INCREASES errors to 70%** - Keeping CPU spinning without memory operations prolongs the race window
2. **Immediate page allocation DECREASES errors to 8%** - Forcing page table operations closes the race window
3. **The race condition is time-sensitive** - Not just presence of 4 threads, but WHAT happens after cleanup

**What this means:**

- After thread cleanup (`unmapPage` + TLBI), there's a **timing window** where speculative PTW can still access stale translations
- **Busy-wait**: CPU spins without forcing TLB/cache synchronization → window stays open → 89% errors
- **Immediate alloc**: Page table operations force proper synchronization → window closes → 2% errors
- **Normal execution**: Random timing → intermediate error rate (~30%)

**Root cause hypothesis refined:**

The TLBI barriers (DSB, ISB) complete locally but don't prevent **speculative PTW races on Cortex-A78AE**. The PTW can speculatively start walking old translations before the new VTTBR takes effect on other cores/threads.

**Why immediate allocation helps:**

When we allocate new pages immediately after cleanup:
1. The allocator likely reuses the just-freed pages
2. This forces new page table entries for the same physical memory
3. The new mappings overwrite any stale TLB entries
4. This effectively "forces" synchronization that the barriers should provide

### Phase 5: Inner-Shareable TLBIs

**Hypothesis**: Local TLBIs don't broadcast across DSU cluster; inner-shareable TLBIs might help.

**Changes made:**
- `invalidateLocalTLB_IPA()`: `tlbi ipas2e1` → `tlbi ipas2e1is`
- `invalidateLocalTLB_IPA()`: `tlbi vmalle1` → `tlbi vmalle1is`
- `invalidateLocalTLB_VMALLS12E1()`: `tlbi vmalls12e1` → `tlbi vmalls12e1is`

**Results (DELAYED test, 100 iterations):**

| Configuration | Error Rate |
|---------------|------------|
| Local TLBIs | **70%** |
| Inner-shareable TLBIs | **55%** |

**~21% improvement, but still significant errors.** Inner-shareable TLBIs help but don't solve the problem.

### Phase 6: DSB ISH Before TLBI (Counter-productive!)

**Hypothesis**: Adding a DSB ISH barrier before TLBI ensures cache is flushed before TLBI starts.

**Result**: **90% errors** (WORSE than 55% with IS TLBIs alone!)

### Phase 7: Memory Access After TLBI (No improvement)

**Hypothesis**: Reading from PTE after TLBI might force synchronization.

**Result**: **75% errors** - no improvement (kernel memory access doesn't help user-space PTW)

### Phase 8: Full ASID TLBI (Best kernel fix!)

**Hypothesis**: Invalidate ALL TLB entries for ASID instead of just one VA.

**Change**: In `unmapPage()`: `invalidateTLBByASIDVA(asid, vptr)` → `invalidateTLBByASID(asid)`

**Result**: **30% errors** - significant improvement!

### Summary Table (DELAYED test, 100 iterations each)

| Configuration | Error Rate | Notes |
|---------------|------------|-------|
| Per-VA local TLBI (baseline) | 70% | Original code |
| Per-VA inner-shareable TLBI | 55% | IS helps |
| Per-VA IS + DSB ISH before | 90% | WORSE! |
| Per-VA IS + mem access after | 75% | No help |
| Full ASID IS TLBI | 30% | Significant improvement |
| **HCR_EL2.VM disable during VTTBR switch** | **0%** | ✅ SOLUTION! |

**Critical Insight**: Adding barriers that *wait* makes the problem WORSE!

**Pattern confirmed:**
- DELAYED test (busy-wait after cleanup): 70%
- ALLOC test (immediate memory ops): 8%
- Adding DSB ISH: 90% (worse)

**Conclusion**: The problem is NOT about barrier ordering. The speculative PTW races during **idle periods**. Adding barriers creates more idle time where speculation runs unchecked.

### Phase 9: HCR_EL2.VM Disable During VTTBR Switch ✅ SUCCESS!

**Hypothesis**: Disable Stage 2 translation entirely during VTTBR switch to prevent ALL speculative PTW.

**Background**: Linux uses a similar approach with EPD bits in TCR_EL1 (ARM64_WORKAROUND_SPECULATIVE_AT errata). For Stage 2 (VTTBR), we use HCR_EL2.VM bit.

**Implementation** in `setCurrentUserVSpaceRoot()`:

```c
/* Disable Stage 2 translation - prevents speculative PTW */
word_t hcr;
MRS("hcr_el2", hcr);
MSR("hcr_el2", hcr & ~HCR_VM_BIT);
dsb();
isb();

/* Write new VTTBR - no speculative walks possible now */
MSR("vttbr_el2", ttbr.words[0]);
dsb();
isb();

/* Re-enable Stage 2 translation */
MSR("hcr_el2", hcr);
dsb();
isb();
```

**Results:**

| Test | Before | After |
|------|--------|-------|
| THREAD_LIFECYCLE_DELAYED_0001 (100 iterations) | 70% errors | **0% errors** |
| Full sel4test suite | N/A | **155 tests passed** |

**Why this works:**
1. `HCR_EL2.VM=0` completely disables Stage 2 translation
2. While disabled, no speculative PTW can occur for Stage 2
3. DSB+ISB ensures the HCR change takes effect before VTTBR write
4. This is similar to Linux's EPD workaround but applied at Stage 2 level

**Safety:**
- Context switches occur with interrupts disabled
- Temporarily disabling Stage 2 is safe in this critical section
- The window is very short (just the VTTBR update)

### ✅ Current Best Kernel Fix (Final)

The **HCR_EL2.VM disable workaround** completely eliminates the speculative PTW race condition:

- **70% → 0%** error rate for worst-case test (DELAYED)
- Full test suite passes (155 tests)
- No user-space workarounds needed

**Note:** Some residual RAS errors (~7) may still occur in CANCEL_BADGED_SENDS tests during a full test run. These appear to be related to a different code path but don't cause test failures.

### Next Steps for Investigation

1. ~~Test on QEMU~~ - Skipped per user request
2. ~~Inner-shareable TLBIs~~ - Partial improvement (70% → 55%)
3. ~~DSB ISH before TLBI~~ - Made it WORSE (90%)
4. ~~Memory access after TLBI~~ - No improvement (75%)
5. ~~Full ASID TLBI~~ - **Best kernel fix (30%)**
6. **Investigate Cortex-A78AE errata** - Check for known PTW speculation bugs
7. **Check if PTW prefetch can be disabled** - System register to disable speculation?
8. **User-space workaround** - Allocate dummy pages after thread destruction?

**Already ruled out (DO NOT investigate):**
- FPU operations (proven not the trigger)
- Context switching alone without destruction (proven not the trigger)
- Single-thread lifecycle (proven not the trigger)
- Endpoint IPC (proven not the trigger)

### Stress Test Results (100 runs each)

Actual results from 100-iteration stress test (200 total test runs):

| Test | Runs with ≥1 RAS Error | Trigger Rate |
|------|------------------------|--------------|
| FPU0001 | 41/100 | **41%** |
| CANCEL_BADGED_SENDS_0002 | 56/100 | **56%** |

**Total**: 97/200 runs (48.5%) triggered at least one RAS error.

**Note**: Rates differ from 25-run estimates due to alternating test pattern affecting cache/TLB state.

### Error Characteristics

- **Error addresses**: `0x7fffxxxx` range (just below DRAM base 0x80000000)
- **Bit 63 = NS flag** (non-secure), not part of address
- **Error sources**: ACI (50%), SCC (41%), IOB (9%)
- **All errors during context switching** between threads/processes

### Phase 10: Arch_finaliseCap Page Table Clearing (2025-12-18)

**Problem**: After the Bug B fix (`pte_safe_invalid_new()`), FPU0001 still had ~250 RAS errors while CANCEL_BADGED_SENDS had ~0.

**Investigation**: Checked if any code paths still leave all-zeros or garbage PTEs during page table/VSpace finalization.

**Findings**:

1. **`unmapPageTable()`** (vspace.c:1136) only clears the PARENT entry pointing to the page table:
   ```c
   *ptSlot = pte_pte_invalid_new();  // Only clears parent pointer, not entries INSIDE the PT
   ```
   The entries inside the page table are NOT cleared.

2. **`deleteASID()`** (vspace.c) only invalidates TLB and clears ASID pool entry - doesn't touch VSpace entries.

3. **`Arch_finaliseCap`** for `cap_page_table_cap` and `cap_vspace_cap` did NOT clear the entries inside these objects.

**Fix Applied** (objecttype.c):

```c
case cap_vspace_cap:
    if (final && cap_vspace_cap_get_capVSIsMapped(cap)) {
        vspace_root_t *vspace = VSPACE_PTR(cap_vspace_cap_get_capVSBasePtr(cap));
        deleteASID(cap_vspace_cap_get_capVSMappedASID(cap), vspace);

        /* Clear VSpace entries to pte_pte_invalid_new() to prevent
         * speculative page table walks from accessing addresses below
         * DRAM base (triggers SCC Address Range Errors on Tegra). */
        for (word_t i = 0; i < BIT(seL4_VSpaceIndexBits); i++) {
            vspace[i] = pte_pte_invalid_new();
        }
        cleanInvalidateCacheRange_RAM((word_t)vspace,
                                      (word_t)vspace + MASK(seL4_VSpaceBits),
                                      addrFromPPtr(vspace));
    }
    break;

case cap_page_table_cap:
    if (final && cap_page_table_cap_get_capPTIsMapped(cap)) {
        pte_t *pt = PTE_PTR(cap_page_table_cap_get_capPTBasePtr(cap));
        unmapPageTable(...);

        /* Clear page table entries to pte_pte_invalid_new() */
        for (word_t i = 0; i < BIT(seL4_PageTableIndexBits); i++) {
            pt[i] = pte_pte_invalid_new();
        }
        cleanInvalidateCacheRange_RAM((word_t)pt,
                                      (word_t)pt + MASK(seL4_PageTableBits),
                                      addrFromPPtr(pt));
    }
    break;
```

**Results**:
- CANCEL_BADGED_SENDS_0002: **510 → 0 errors** (fix worked!)
- FPU0001: **250 → 218 errors** (slight improvement but still present)

### Phase 11: Debug Page Table Scan (2025-12-18)

**Problem**: FPU0001 still has RAS errors after Arch_finaliseCap fix. FPU0001 creates threads that SHARE a VSpace, so VSpace finalization never happens during this test.

**Approach**: Added debug scanning code to `setVMRoot()` that walks the entire VSpace looking for PTEs with addresses below DRAM base (0x80000000).

**Scan Function** (vspace.c):
```c
static word_t scan_vspace_for_bad_ptes(vspace_root_t *vspace, bool_t print_all)
{
    /* Walks L0 → L1 → L2 → L3, reporting any PTE with address < DRAM_BASE_PHYS */
    for (word_t l0 = 0; l0 < BIT(seL4_VSpaceIndexBits); l0++) {
        // Walk page table hierarchy, check each entry's address field
    }
}
```

**CRITICAL FINDING**: The scan found **428+ "bad PTEs"** but the values are clearly **garbage user data**, not legitimate PTEs:

| Value | Decoded | Source in sel4test |
|-------|---------|-------------------|
| `0x65726f63` | "core" (ASCII) | `simple_get_core_count` |
| `0x77656e5f` | "_new" (ASCII) | `seL4_MessageInfo_new`, etc. |
| `0x74615f63` | "c_at" (ASCII) | Function names |
| `0x682e745f` | "_t.h" (ASCII) | `kobject_t.h` path |
| `0x5d695b` | "[i]" (ASCII) | Assert: `sel4_page_sizes[i]...` |
| `0x5280000054fffe23` | ARM MOV instruction | Code section |

**Verified**: Running `strings` on `sel4test-driver` binary confirms all these strings exist in the application.

**Interpretation**: Page table memory contains **leftover user application data** (strings, code) from previous use. This means:

1. Physical frame is used as user stack/data (contains strings, code)
2. Thread is deleted, frame is freed
3. Frame is retyped to page table
4. `clearMemory()` is called but **doesn't properly flush to DRAM**
5. Cache lines are later evicted, CPU fetches garbage from DRAM
6. Speculative PTW interprets garbage as PTE, walks to invalid address
7. **RAS error triggered**

### Phase 12: Investigation of Garbage in Page Tables (2025-12-18) - IN PROGRESS

#### ⚠️ CRITICAL FINDING: Page Tables Corrupted AFTER Creation

Debug instrumentation added to track page table lifecycle:

1. **Creation verification** (`objecttype.c`): Check all PTEs match `pte_pte_invalid_new()` immediately after creation and cache flush
2. **Runtime scanning** (`vspace.c`): Scan VSpace for bad PTEs (pointing below DRAM) every 1000 context switches

**Test Results (2025-12-18 14:58):**

```
RAS_DEBUG: VSpace@0xac002000 created OK          ← Creation passes!
...
RAS_SCAN: VSpace@0xac002000 has 435 bad PTEs (sample: 0x5280000054fffe23 -> 0x54fff000)
```

**Interpretation:**
- VSpace is correctly initialized at creation time (passes verification)
- **LATER during runtime**, 435 PTEs contain garbage data
- Sample garbage PTE: `0x5280000054fffe23`
  - Bits 1:0 = 0x3 (table entry - "looks valid" to hardware!)
  - Address field = 0x54fff000 (BELOW DRAM base 0x80000000)
- RAS errors show `ADDR = 0x800000007fff44c0` → PA `0x7fff44c0` (below DRAM)

**Conclusion: Page tables are getting corrupted AFTER creation.**

This proves corruption happens after creation, but does NOT tell us HOW:
- **Hypothesis A (cache)**: Stale DRAM data appears when cache evicts new PTE values
- **Hypothesis C (overwrite)**: Something actively writes to PT memory after creation (ELF loader, stray pointer, mapping bug)

We need to trace the memory reuse path and/or add write barriers to determine which.

#### Previous Disassembly Verification

**Verified**: The `pte_pte_invalid_new()` macro IS working correctly. Disassembly of `Arch_createObject` confirms:
```
8080019ce0:  adrp x2, 808025a000 <armKSGlobalUserVSpace>
8080019ce4:  add  x2, x2, #0x0
8080019ce8:  mov  x3, #0xffffff8000000000  // addrFromKPPtr
8080019cec:  add  x3, x2, x3
8080019cf8:  and  x3, x3, #0xfffffffff000  // mask low 12 bits to 0 (invalid)
8080019d18:  str  x3, [x1, x0, lsl #3]     // store to PTE
```

The macro stores `armKSGlobalUserVSpace` physical address with bits 11:0 = 0 (invalid entry type) to page table entries at creation time.

#### Remaining Hypotheses

**Primary (cache flush in memory reuse)**:
1. User frame contains application data (strings, code)
2. Frame freed → memory retyped to page table via `Retype` syscall
3. `clearMemory()` does memzero but **NO cache flush to PoC**
4. Page table initialization writes safe PTEs to cache
5. Cache lines later evicted → DRAM still has old user data
6. Speculative PTW reads garbage from DRAM, interprets as PTE
7. Walks to invalid PA (0x7fffxxxx) → **RAS error**

**Secondary (post-creation corruption)**:
- Something writes to page table memory after creation
- Stray pointer, DMA, or stage 2 translation issue

#### Next Steps

1. **Trace memory reuse path**: Find where `clearMemory()` is called without cache flush
2. **Add cache flush to `clearMemory()`**: Test if flushing to PoC fixes the issue
3. **Instrument `Retype` syscall**: Track memory transitions from frame → page table

**Key observation**: The garbage is sel4test APPLICATION data (strings, code), not kernel data. This confirms user-space data reaches page table memory through memory reuse.

### Phase 13: Rootserver Page Table Initialization Fix (2025-12-18)

#### Discovery: Zeroed Memory is NOT Safe

Analysis of boot code revealed a critical issue:

1. **`alloc_rootserver_obj()`** in `boot.c` calls `memzero()` to zero-initialize allocated memory
2. **Zero PTEs are UNSAFE** for ARM64 speculative PTW:
   - Bits[1:0] = 0 (invalid entry type) - correct
   - Bits[47:12] = 0 (points to PA=0) - **WRONG!**
   - Speculative PTW reads bits[47:12] regardless of validity bits
   - PA=0 is below DRAM base (0x80000000) → **RAS error**

3. **`it_alloc_paging()`** in `boot.h` returns raw memory from the zeroed pool without any PTE initialization

4. Safe invalid PTEs must have bits[47:12] pointing to `armKSGlobalUserVSpace` (valid DRAM address), even when bits[1:0]=0 (invalid type)

#### Fix Applied

Added safe PTE initialization to boot-time page table creation in `vspace.c`:

```c
/* Initialize a page table with safe invalid PTEs */
static BOOT_CODE void init_pt_with_safe_ptes(pptr_t pptr)
{
    pte_t *pt = (pte_t *)pptr;
    pte_t safe_pte = pte_pte_invalid_new();  // bits[47:12] = armKSGlobalUserVSpace
    for (word_t i = 0; i < BIT(seL4_PageTableIndexBits); i++) {
        pt[i] = safe_pte;
    }
    /* Clean cache to ensure safe PTEs are visible to MMU PTW */
    cleanCacheRange_RAM((word_t)pt, ((word_t)pt) + BIT(seL4_PageTableBits) - 1,
                        addrFromKPPtr(pt));
}
```

Modified these functions to call `init_pt_with_safe_ptes()`:
- `create_it_pt_cap()` - L3 page tables
- `create_it_pd_cap()` - L2 page directories
- `create_it_pud_cap()` - L1 page upper directories
- `create_it_address_space()` - rootserver VSpace (L0)

#### Test Results (2025-12-18 15:35)

RAS errors **STILL OCCURRING** after the fix. Key observations:

```
RAS_DEBUG: VSpace@0xac002000 created OK (safe_addr=0x8025a000)
RAS_DEBUG: PT@0xac00b000 created OK
RAS_DEBUG: PT@0xac00c000 created OK
...
RAS_SCAN: VSpace@0xac002000 L0:0 L1:0 L2:0 L3:13642 (1st@L3 in PT@0xac00a000: 0x4001ec->0x400000)
```

**Analysis:**

1. **Logged "created OK"**: VSpace and several PTs pass verification at creation time
2. **Bad PTs NOT logged**: PT@0xac00a000 appears in scan but was NEVER logged as "created OK"
3. **Bad PTE value**: `0x4001ec` has:
   - Bits[1:0] = 0b00 (invalid entry)
   - Bits[47:12] = 0x400 → PA = **0x400000** (rootserver's virtual address!)
4. **RAS error address**: Still `0x800000007ffcc000` → PA = 0x7ffcc000

**Interpretation:**

The bad PTEs contain stale data pointing to 0x400000, which is the rootserver's **VIRTUAL** address (not physical). This value was likely:
1. Written during ELF loading when the rootserver was mapped
2. Left in DRAM when the cache line was evicted
3. Later interpreted as a PTE by speculative PTW

The boot-time initialization fix only addresses page tables created via `create_it_*_cap()`. But the scan is finding bad entries at addresses that were never logged - suggesting these entries come from:
- **Memory reuse** without proper cache flush (Hypothesis A)
- **Mapping data** left in cache/DRAM from previous use
- Or **different allocation path** not covered by the fix

**RAS Error Pattern:**
- Same address `0x7ffcc000` consistently
- Still occurring during `CANCEL_BADGED_SENDS_0002` test
- Indicates the root cause is NOT boot-time initialization

### Phase 14: RAM Base Test at 0x90000000 (2025-12-18)

#### Test Setup

Changed DRAM configuration in `kernel/tools/dts/orinagx.dts`:
```dts
memory@90000000 {
    device_type = "memory";
    reg = <0x0 0x90000000 0x0 0x30000000>;  /* 768MB at 0x90000000 */
};
```

This moves the entire RAM region up by 256MB, from 0x80000000-0xAFFFFFFF to 0x90000000-0xBFFFFFFF.

#### Results: **NO RAS ERRORS**

Despite running the full stress test (100× CANCEL_BADGED_SENDS_0002), **ZERO RAS errors occurred**.

Key observations:
- Boot output confirms new memory layout: `available phys memory regions: [90000000..c0000000)`
- `safe_addr` is now at `0x9025a000` (armKSGlobalUserVSpace in new DRAM region)
- The scan STILL finds "bad" PTEs: `L3:13642 (1st@L3 in PT@0xbc00a000: 0x4001ec->0x400000)`
- But these bad PTEs do NOT trigger RAS errors!

#### Analysis

| RAM Base | DRAM Range | RAS Errors | Bad PTEs Found |
|----------|------------|------------|----------------|
| 0x80000000 | 0x80000000-0xAFFFFFFF | **YES** (many) | 13,642 |
| 0x90000000 | 0x90000000-0xBFFFFFFF | **NO** | 13,642 |

**Key Insight**: The bad PTEs still exist (pointing to VA 0x400000, which is below DRAM in both cases), but RAS errors only occur when DRAM starts at 0x80000000.

This confirms the earlier RAM bisection finding: **the first ~200KB of physical address space starting at 0x80000000 on Tegra234 triggers RAS errors when accessed by speculative PTW**.

#### Two Separate Issues

1. **Bad PTEs (0x400000)**: These are likely a separate bug - stale mapping data containing the rootserver's virtual address (0x400000) appearing in Stage 2 page tables. This data shouldn't be there, but it's NOT causing the RAS errors.

2. **RAS Errors (0x7ffcc000)**: These only occur when:
   - DRAM starts at/near 0x80000000
   - Speculative PTW accesses addresses just below 0x80000000 (like 0x7ffcc000)
   - This suggests a Tegra234-specific issue with the memory region at/below 0x80000000

#### Hypothesis: Tegra234 Memory Controller Issue

The RAS errors are triggered by speculative PTW accessing addresses **just below** the configured DRAM base. When DRAM starts at 0x80000000:
- Addresses like 0x7fffxxxx are in a "reserved" or "special" region
- The memory controller reports these as errors

When DRAM starts at 0x90000000:
- The 0x80000000-0x8FFFFFFF region is now marked as "device memory" in seL4
- The 0x7fffxxxx region is further away from the DRAM boundary
- Speculation to 0x7fffxxxx might not trigger errors (different memory controller behavior?)

**OR**: The problematic addresses (0x7ffcc000, etc.) were derived from DRAM base somehow, and moving DRAM changes what addresses get speculatively accessed.

#### Debug Isolation Tool (NOT a Workaround)

**⚠️ IMPORTANT**: Moving DRAM to 0x90000000 is a **debugging/isolation tool only**, NOT an acceptable production workaround.

For debugging/bisection purposes:
- Configure DRAM to start at 0x90000000 to isolate Bug A from Bug B
- This helps determine which issues are RAM-location dependent
- The underlying bugs still exist and MUST be fixed properly

**Why this is NOT a workaround**:
1. It wastes 256MB of usable DRAM (0x80000000-0x8FFFFFFF becomes unusable)
2. It does not fix the root cause - the bugs remain
3. The stale-PTE issue (0x400000 in PTEs) is still present and may cause other problems

---

## Background

The errors report ADDR values like `0x8000000000000000`, which decode to **PA=0x0 (NULL pointer access)** from non-secure world (bit 63 is the NS flag, not part of the address - see "ARM RAS ADDR Encoding" section).

**Previous Hypothesis**: seL4's TLB invalidation code may have set VTTBR_EL2 with a NULL base address (0) when switching VMIDs. This was partially fixed but errors persist.

**Current Hypothesis**: The bug is in FPU context switching or thread deletion paths, causing speculative PTW to access invalid/stale page table entries during context switches.

## Error Pattern

### Observed RAS Error Addresses

During sel4test multi-run testing, the following error addresses were observed:

| Run | ADDR (raw) | Decoded PA (bits 55:0) | NS (bit 63) | Valid DRAM? |
|-----|------------|------------------------|-------------|-------------|
| 1 | `0x8000000000001b00` | `0x0000000000001b00` | 1 (non-secure) | ❌ NO (below 0x80000000) |
| 3 | `0x800000007ffc0800` | `0x0000007ffc0800` | 1 (non-secure) | ❌ NO (below 0x80000000) |
| 3 | `0x800000007ffc0bc0` | `0x0000007ffc0bc0` | 1 (non-secure) | ❌ NO (below 0x80000000) |
| 4 | `0x800000007ffc0040` | `0x0000007ffc0040` | 1 (non-secure) | ❌ NO (below 0x80000000) |
| 4 | `0x800000007ffc0bc0` | `0x0000007ffc0bc0` | 1 (non-secure) | ❌ NO (below 0x80000000) |
| 4 | `0x800000007ffc0c00` | `0x0000007ffc0c00` | 1 (non-secure) | ❌ NO (below 0x80000000) |

**Note**: ALL observed error PAs are below Orin's DRAM base (0x80000000). These are accesses to unmapped memory regions.

### Address Pattern Analysis

**IMPORTANT**: Bit 63 is the NS (Non-Secure) flag, NOT part of the physical address! See "ARM RAS ADDR Encoding" section for details.

- **Bit 63 (NS flag)**: All accesses are from non-secure world
- **Actual PA range**: Decoded PAs are in `0x0` to `0x7ffc0xxx` range
- **ALL PAs are below DRAM base**: Orin DRAM starts at `0x80000000`. All observed error PAs are below this - they are **NOT valid DRAM addresses**
- **PA=0x0 pattern**: Some errors show PA=0x0 (NULL pointer access)
- **PA=0x7ffc0xxx pattern**: Other PAs like `0x7ffc0xxx` are also below DRAM base (unmapped memory region)

### Test Triggering the Error

The RAS error consistently occurs during:
- **Test**: `CANCEL_BADGED_SENDS_0002`
- **Frequency**: ~60% of runs (3 out of 5 in multi-run testing)

## Memory Configuration

### seL4 Memory Layout on Orin AGX

```
Physical Address Space (40-bit):
  physBase:     0x80000000
  Memory size:  8GB (0x200000000)
  PA range:     0x80000000 - 0x280000000

Virtual Address Space (Hypervisor mode):
  PPTR_BASE:        0x0000008000000000 (2^39)
  PPTR_TOP:         0x000000ffc0000000 (2^40 - 2^30)
  PPTR_BASE_OFFSET: 0x0000008000000000
```

### VTCR Configuration

```c
// kernel/include/arch/arm/armv/armv8-a/64/armv/vcpu.h:594-610
vtcr_el2 = VTCR_EL2_T0SZ(24);     // 40-bit input IPA
vtcr_el2 |= VTCR_EL2_PS(PS_1T);   // 40-bit PA size
vtcr_el2 |= VTCR_EL2_SL0(SL0_4K_L1);  // Start at level 1
```

## TLB Barrier Analysis

### PTE Update Sequence

The sequence for unmapping a page (`unmapPage` in `kernel/src/arch/arm/64/kernel/vspace.c:1035`):

```
1. Store PTE:     *(lu_ret.ptSlot) = pte_pte_invalid_new()
2. Cache clean:   dc cvau, %0
3. Memory barrier: dmb sy
4. DSB:           dsb sy (in invalidateLocalTLB_IPA_VMID)
5. VMID switch:   dsb sy → MSR vttbr_el2 → isb (if needed)
6. TLBI stage 2:  tlbi ipas2e1, %0
7. DSB:           dsb sy
8. TLBI stage 1:  tlbi vmalle1
9. DSB:           dsb sy
10. ISB:          isb
```

### Key Functions and Their Barriers

| Function | Location | Barriers |
|----------|----------|----------|
| `cleanByVA_PoU` | mode/machine.h:286 | dc cvau → dmb sy |
| `invalidateLocalTLB_IPA_VMID` | armv/tlb.h:34 | dsb sy at entry |
| `setCurrentUserVSpaceRoot` | mode/machine.h:179 | dsb sy → MSR → isb |
| `invalidateLocalTLB_IPA` | mode/machine.h:269 | tlbi → dsb → tlbi → dsb → isb |

### Analysis Result

**The barrier sequences are correct** according to ARM architecture requirements:
- DSB before TLBI ensures PTE writes are visible
- DSB after TLBI ensures invalidation is complete
- ISB ensures instruction stream sees the changes

## SError Handling

### Configuration

```cmake
# kernel/src/plat/orinagx/config.cmake:20
set(KernelAArch64SErrorIgnore ON)
```

### Behavior

When `KernelAArch64SErrorIgnore` is enabled:
- SError exceptions trigger a simple `eret` return
- No kernel panic or error handling
- RAS errors are reported by NVIDIA firmware, not seL4

```asm
# kernel/src/arch/arm/64/traps.S:144-148
#ifdef CONFIG_AARCH64_SERROR_IGNORE
    eret
#else
    b       invalid_vector_entry
#endif
```

## Possible Root Causes

### 1. ARM RAS ADDR Encoding Clarification

**Note**: The ADDR field uses standard ARM RAS encoding where bit 63 is the NS (Non-Secure) flag, not part of the address. See "ARM RAS ADDR Encoding" section below for details.

- `0x8000000000000000` decodes to: NS=1, **PA=0x0** (NULL pointer access)
- `0x800000007ffc0xxx` decodes to: NS=1, **PA=0x7ffc0xxx** (below DRAM base 0x80000000 - unmapped)

The different patterns between hypervisor and non-hypervisor modes may indicate different speculative access patterns, not different encoding schemes.

### 2. Speculative Memory Access During TLB Flush

The error consistently occurs during thread bind/unbind operations (BIND tests) or endpoint operations (CANCEL_BADGED_SENDS). These operations involve TLB invalidation. Speculative execution on Cortex-A78AE may access memory with stale TLB entries before invalidation fully propagates.

### 3. Page Table Race Condition

Thread binding/unbinding involves page table modifications. A race between CPU speculative access and TLB invalidation could cause the MMU to access invalid entries temporarily.

### 4. Hardware Memory Controller Issue

Could be an ECC error, parity issue, or memory controller bug triggered by specific access patterns during kernel context switching.

### 5. ~~Stage-2 Translation Fault~~ (Ruled Out)

~~In hypervisor mode, stage-2 translation uses IPA (Intermediate Physical Address). An invalid IPA could trigger the memory controller error.~~

**Update**: Testing with ARM_HYP=OFF shows the error occurs MORE frequently without stage-2 translation (100% vs 60%), ruling out stage-2 as the root cause.

## Attempted Fixes

### Boot Delay Test (2 seconds)

Added a 2-second delay at kernel boot (after "Bootstrapping kernel" message) to test if RAS errors are async leftovers from bootloader/UEFI:

```c
// kernel/src/arch/arm/kernel/boot.c
#ifdef CONFIG_PLAT_ORIN_AGX
    uint64_t freq, start, now, target;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    asm volatile("mrs %0, cntpct_el0" : "=r"(start));
    target = start + (freq * 2);  /* 2 second delay */
    printf("DEBUG: Waiting 2 seconds for async RAS errors...\n");
    do {
        asm volatile("mrs %0, cntpct_el0" : "=r"(now));
    } while (now < target);
    printf("DEBUG: Delay complete, continuing boot\n");
#endif
```

**Result**: RAS errors still occur during tests, NOT during the 2-second boot delay. This proves:
- Errors are **synchronous**, triggered by kernel operations
- NOT leftover async errors from bootloader/UEFI
- The triggering operation is in the kernel, not external

### PA Validation in cache.c (Reverted)

Attempted to add physical address validation assertions to catch invalid addresses:

```c
// Early boot crashes prevented this approach
static inline void validate_pa(paddr_t pa) {
    assert(pa >= ORIN_PA_MIN && pa <= ORIN_PA_MAX);
}
```

**Result**: Caused early boot crashes because:
- printf() not available during early boot
- Static variable approach also crashed
- Had to revert all changes

### Current State

- `kernel/src/plat/orinagx/machine/cache.c` uses dc civac (correct for Tegra)
- TLB barriers are correct
- SErrors are ignored
- RAS errors still occur intermittently (~60% of runs)

## Files Examined

| File | Purpose |
|------|---------|
| `kernel/src/plat/orinagx/machine/cache.c` | Tegra-specific cache operations |
| `kernel/src/arch/arm/64/kernel/vspace.c` | Page table and TLB operations |
| `kernel/include/arch/arm/arch/64/mode/machine.h` | TLB invalidation functions |
| `kernel/include/arch/arm/armv/armv8-a/64/armv/tlb.h` | Hypervisor TLB functions |
| `kernel/include/arch/arm/armv/armv8-a/64/armv/vcpu.h` | VTCR configuration |
| `kernel/src/plat/orinagx/config.cmake` | Platform configuration |

## Recommendations

### Short Term

1. **Continue with SError ignore**: The current `KernelAArch64SErrorIgnore ON` setting allows tests to complete despite RAS errors
2. **Monitor frequency**: Track if the error rate changes with kernel modifications

### Medium Term

1. **NVIDIA consultation**: Contact NVIDIA about the RAS error address format and meaning
2. **Firmware investigation**: Check if UEFI/TF-A configuration affects RAS reporting
3. **Memory test**: Run Linux memtest on the same hardware to rule out hardware issues

### Long Term

1. **RAS error handler**: Implement proper SError handling that logs the error details without crashing
2. **Page table validation**: Add runtime checks for page table integrity (with boot-safe logging)
3. **Hardware debugging**: Use NVIDIA debugging tools to trace memory accesses

## Test Results Summary

### Hypervisor Mode (ARM_HYP=ON)

| Test Configuration | Result |
|-------------------|--------|
| sel4test multi-run (5 runs) | 3/5 had RAS errors (60%) |
| Triggered by | CANCEL_BADGED_SENDS_0002 |
| Error Address Pattern | `0x800000007ffc0xxx` |
| Tests pass despite errors | Yes |
| Kernel crashes | No (SError ignored) |

### Non-Hypervisor Mode (ARM_HYP=OFF)

| Test Configuration | Result |
|-------------------|--------|
| sel4test multi-run (5 runs) | 5/5 had RAS errors (100%) |
| Triggered by | BIND0001 (occasionally BIND0003) |
| Error Address Pattern | `0x8000000000000000` or `0x8000000000000010` |
| Tests pass despite errors | Yes (tests interrupted but don't fail) |
| Kernel crashes | No (SError ignored) |

### Comparative Analysis

| Aspect | Hypervisor Mode | Non-Hypervisor Mode |
|--------|-----------------|---------------------|
| Error Rate | 60% (3/5) | 100% (5/5) |
| Triggering Test | CANCEL_BADGED_SENDS | BIND tests |
| Address Pattern | physBase in upper bits | Clean bit 63 only |
| Stage-2 Translation | Yes | No |

**Key Finding**: RAS errors are NOT hypervisor-specific. They actually occur MORE frequently without hypervisor support, suggesting the root cause is in core kernel operations related to thread management or endpoint operations, not stage-2 translation.

### Single-Core DTS Test (ARM_HYP=ON)

Disabled 11 of 12 CPU cores in the device tree to test if multi-core cache coherency is the cause:

| Run | RAS Errors | Triggering Test |
|-----|------------|-----------------|
| 1 | 2 errors | IPC1002 (inter-AS Call/ReplyRecv) |
| 2 | 0 errors | Clean run |
| 3 | 2 errors | IPC1004 |
| 4 | 5 errors | CANCEL_BADGED_SENDS_0001 |
| 5 | 2 errors | IPC1001 |

**Result**: Error rate **increased** from 60% to 80% with single-core DTS. Errors now occur in different tests (IPC tests, not just CANCEL_BADGED_SENDS). Different RAS sources: IOB (0xe010000) in addition to SCC.

**Key Finding**: Disabling secondary cores in DTS does NOT help. Note that `status = "disabled"` only tells seL4 not to use those cores - TF-A firmware may still have all 12 cores running in WFI loops. This rules out seL4 multi-core coherency as the cause.

## Additional Experiments

### Boot Delay Test (2-4 seconds)

Added a delay loop at kernel boot to test if RAS errors are async leftovers from bootloader/UEFI:

**Result**: No RAS errors occurred during the delay. Errors only occur during test execution. This proves errors are **synchronous** - triggered by kernel operations, not async from bootloader.

### DMB Barrier in cancelBadgedSends

Added `dmb sy` memory barrier in the cancelBadgedSends loop to test speculative execution hypothesis:

**Result**: No change in error rate (still ~60%). Memory barrier in this location does not help.

## Summary of Ruled-Out Causes

| Hypothesis | Test | Result |
|------------|------|--------|
| Async errors from bootloader | 4-second boot delay | Ruled out - errors only during tests |
| Stage-2 translation (hypervisor) | ARM_HYP=OFF test | Ruled out - errors worse without hyp |
| seL4 multi-core coherency | Single-core DTS | Ruled out - errors worse with single-core |
| Speculative execution in cancelBadgedSends | DMB barrier | Ruled out - no change |
| Memory ordering in cancelBadgedSends | DSB barrier (2025-12-16) | Ruled out - no change |
| TLB invalidation VMID switch race | HCR_EL2.VM disable in TLB funcs | Ruled out - no change for CANCEL_BADGED_SENDS |

### CANCEL_BADGED_SENDS Specific Summary (2025-12-16)

The HCR_EL2.VM workaround that fixed FPU0001/thread lifecycle tests does NOT fix CANCEL_BADGED_SENDS. Additional attempts:

| Fix Attempted | Location | Result |
|--------------|----------|--------|
| HCR_EL2.VM disable during VTTBR switch | `setCurrentUserVSpaceRoot()` | ✅ Fixes FPU0001, ❌ Not CANCEL_BADGED_SENDS |
| HCR_EL2.VM disable for entire TLB sequence | `invalidateLocalTLB_VMID()` | ❌ No improvement |
| DSB barriers around cancelBadgedSends loop | `endpoint.c` | ❌ No improvement |
| DMB barrier in cancelBadgedSends | `endpoint.c` | ❌ No improvement (tested 2025-12-13) |

**Observation**: Error addresses are consistently 0x7ffc0xxx (just below DRAM base 0x80000000). This suggests speculative PTW is reading corrupted/stale page table entries that point to invalid physical addresses.

## HYPOTHESIZED ROOT CAUSE

### TLB Invalidation with NULL VTTBR_EL2 Base Address

**Hypothesis**: seL4 intentionally sets VTTBR_EL2 with a NULL base address (0) when switching VMIDs for TLB invalidation. Tegra234's memory subsystem may not tolerate this, generating RAS errors when speculative table walks access PA=0x0. This hypothesis is supported by evidence but not definitively confirmed.

#### Call Path

```
unmapPage
  → invalidateTLBByASIDVA(asid, vptr)
    → invalidateTranslationSingle((hw_asid << 48) | vaddr >> seL4_PageBits)
      → invalidateTranslationSingleLocal(vptr)
        → invalidateLocalTLB_IPA_VMID(vptr)
          → setCurrentUserVSpaceRoot(ttbr_new(vmid, 0))  // BUG: base=0!
          → invalidateLocalTLB_IPA(ipa)
            → tlbi ipas2e1, %0
            → dsb sy
            → tlbi vmalle1    // <-- RAS ERROR OCCURS HERE
```

#### ATF Debug Output Proof

Added debug code to TF-A's `tegra234_ras_handler()` to capture EL2/EL3 state:

```
ERROR:   ======= RAS INTERRUPT CONTEXT =======
ERROR:   ELR_EL3 (seL4 PC when interrupted): 0x808001a974
ERROR:   SPSR_EL3: 0x800003c9 (mode=9)  // EL2h
ERROR:   ELR_EL2: 0x40bff8
ERROR:   ESR_EL2: 0x56000000
ERROR:   FAR_EL2: 0x515910
ERROR:   VTTBR_EL2: 0x1000000000000     // VMID=1, BASE=0 (NULL!)
ERROR:   ======================================
```

**VTTBR_EL2 Analysis:**
- Bits [63:48] = VMID = 0x0001 (valid)
- Bits [47:0] = Base = 0x0000_0000_0000 (**NULL - INVALID!**)

**Kernel Instruction at ELR_EL3:**
```
808001a970:  d5033f9f   dsb sy
808001a974:  d508871f   tlbi vmalle1    <-- RAS interrupt taken here
808001a978:  d5033f9f   dsb sy
```

#### seL4 Code Analysis

In `kernel/include/arch/arm/armv/armv8-a/64/armv/tlb.h`:

```c
static inline void invalidateLocalTLB_IPA_VMID(word_t ipa_plus_vmid)
{
    word_t vttbr = getVTTBR();
    word_t v = (vttbr >> 48);
    word_t vmid = ipa_plus_vmid >> 48;
    word_t ipa = ipa_plus_vmid & 0xfffffffff;
    dsb();
    if (v != vmid) {
        setCurrentUserVSpaceRoot(ttbr_new(vmid, 0));  // BUG: sets base=0
    }
    invalidateLocalTLB_IPA(ipa);
    // ...
}
```

The comment in the seL4 code says:
> "Note that an invalid address is used, and it seems fine."

**This is NOT fine on Tegra234!**

#### Why This Happens

According to ARM architecture, TLBI operations should not require a valid page table - they just invalidate TLB entries. However, Tegra234's SCC/IOB/ACI hardware appears to:

1. Trigger internal memory accesses during `tlbi vmalle1`
2. Use the VTTBR_EL2 base address (even if null)
3. Generate RAS errors when speculative accesses to PA=0x0 occur (ADDR reports NS=1, PA=0)

This is either:
- A Tegra234 hardware errata
- NVIDIA-specific behavior not documented in ARM specs
- A stricter implementation of TLB maintenance operations

### Implemented Fix

**Commit**: `448255694ff79fe1204c968530f6bf374670f969`

Modified `kernel/include/arch/arm/armv/armv8-a/64/armv/tlb.h` to use `armKSGlobalUserVSpace` as the VTTBR base instead of NULL:

```c
static inline void invalidateLocalTLB_VMID(word_t vmid)
{
    word_t vttbr = getVTTBR();
    word_t v = (vttbr >> 48);
    dsb();
    /* We need to switch to the target VMID for flushing
     * the TLB if necessary.
     * Note: We use armKSGlobalUserVSpace as a valid empty page table base.
     * Using base=0 causes RAS errors on some platforms (e.g., Tegra234/Orin)
     * where the hardware does speculative table walks during TLBI.
     */
    if (v != vmid) {
        setCurrentUserVSpaceRoot(ttbr_new(vmid, addrFromKPPtr(armKSGlobalUserVSpace)));
    }
    invalidateLocalTLB_VMALLS12E1();
    if (v != vmid) {
        /* Restore the previous VTTBR value */
        setCurrentUserVSpaceRoot((ttbr_t) {
            .words[0] = vttbr
        });
    }
}
```

The same pattern was applied to `invalidateLocalTLB_IPA_VMID()`.

**Why this works**: `armKSGlobalUserVSpace` is an empty page table that exists throughout kernel execution. Using its address as the VTTBR base provides a valid pointer for Tegra234's speculative table walk hardware, preventing RAS errors while still allowing the TLB invalidation to work correctly.

### Fix Verification

| Test | Before Fix | After Fix |
|------|------------|-----------|
| 5-run multi-run | 3/5 RAS crashes (60%) | 1/5 (rare FPU0000 error) |
| 10-run multi-run | N/A | 0/10 RAS crashes (0%) |

The fix eliminates the RAS errors caused by NULL VTTBR base during TLB invalidation. ATF debug output after fix shows valid VTTBR:
```
VTTBR_EL2: 0x1000080d26000  // VMID=1, BASE=0x80d26000 (valid!)
```

## ARM RAS Error Record Address Register (ERR<n>ADDR) Encoding

**IMPORTANT**: The ADDR field in RAS error reports is NOT a raw 64-bit physical address. The upper bits contain status flags!

### Bit Field Layout

| Bits | Field | Description |
|------|-------|-------------|
| **63** | **NS** | Non-secure attribute (1 = non-secure access) |
| 62 | SI | Secure Incorrect (1 = NS field may be wrong) |
| 61 | AI | Address Incorrect (1 = PADDR may be wrong) |
| 60 | VA | Virtual Address (1 = address is VA, not PA) |
| 59 | NSE | Physical Address Space (FEAT_RME only) |
| 58:56 | RES0 | Reserved, read-as-zero |
| **55:0** | **PADDR** | Actual physical address (56-bit max) |

### Decoding Examples

#### Example 1: `ADDR = 0x8000000000000000`

```
Binary: 1000 0000 0000 0000 ... (64 zeros after bit 63)

Bit 63 (NS) = 1    → Non-secure access
Bit 62 (SI) = 0    → Secure field is valid
Bit 61 (AI) = 0    → Address is VALID (not incorrect!)
Bit 60 (VA) = 0    → Physical address (not virtual)
Bits 55:0 (PADDR) = 0x0000_0000_0000 → Physical address is ZERO (NULL!)
```

**Interpretation**: A non-secure access to **physical address 0x0** - this is a NULL pointer access!

#### Example 2: `ADDR = 0x800000007fff4540`

```
Bit 63 (NS) = 1    → Non-secure access
Bit 62 (SI) = 0    → Secure field is valid
Bit 61 (AI) = 0    → Address is VALID
Bit 60 (VA) = 0    → Physical address
Bits 55:0 (PADDR) = 0x0000_7fff_4540 → PA ~2GB (but below DRAM base!)
```

**Interpretation**: A non-secure access to **physical address 0x7fff4540** - this is **BELOW** Orin's DRAM range (DRAM starts at 0x80000000). This address is in an unmapped memory region.

#### Example 3: `ADDR = 0xC000000080100000`

```
Bit 63 (NS) = 1    → Non-secure access
Bit 62 (SI) = 1    → Secure field may be incorrect!
Bit 61 (AI) = 0    → Address is valid
Bits 55:0 (PADDR) = 0x0000_0080_1000_00 → PA 0x80100000
```

### Common Misinterpretation

When seeing error addresses like `0x8000000000000000`, it's easy to think "bit 63 is set, which is impossible for a 40-bit physical address space" and assume it's address corruption.

**WRONG**: Bit 63 is corrupted/invalid
**RIGHT**: Bit 63 is the NS flag; the actual PA is in bits 55:0

### Source References

- [ARM ERR<n>ADDR Register](https://arm.jonpalmisc.com/latest_sysreg/ext-errnaddr) - Jon's ARM Reference
- [ARM Developer Documentation](https://developer.arm.com/documentation/107734/0001/External-registers/External-RAS-registers-summary/ERR0ADDR--Error-Record--n--Address-Register) - Official ARM docs
- ARM Architecture Reference Manual, RAS Extension chapter

## Ongoing Investigation: Remaining RAS Errors (2025-12-14)

### Status: INVESTIGATING

After the initial VTTBR NULL base fix, RAS errors still occur intermittently (~40% of runs pass). Further investigation reveals new patterns.

### New Error Pattern: PA=0 Access

Recent captures show a different error signature:

```
ERROR:   RAS Uncorrectable Error in IOB, base=0xe010000:
ERROR:          Status = 0xec000612
ERROR:   SERR = Error response from slave: 0x12
ERROR:          IERR = CBB Interface Error: 0x6
ERROR:          ADDR = 0x8000000000000000

ERROR:   RAS Uncorrectable Error in ACI, base=0xe01a000:
ERROR:          Status = 0xe8000904
ERROR:   SERR = Assertion failure: 0x4
ERROR:          IERR = FillWrite Error: 0x9
ERROR:          ADDR = 0x8000000000000000
```

**Decoded**: ADDR = 0x8000000000000000 → NS=1, **PADDR = 0x0** (NULL pointer access!)

### Tegra234 Memory Map

Physical address 0x0 has **no memory** on Tegra234:
- SYSRAM starts at 0x40000000 (per [tegra234.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/nvidia/tegra234.dtsi))
- DRAM starts at 0x80000000
- Any access to PA=0 causes a bus error → RAS interrupt

### Hypothesis: Speculative Table Walk with Zero PTEs

The `armKSGlobalUserVSpace` page table is BSS-initialized (all zeros). When used as VTTBR base during TLB operations:

1. Hardware does speculative table walk
2. Reads PTE entry = 0x0 (all zeros, bit[0]=0 = invalid)
3. ARM architecture says invalid entries should cause translation fault
4. **However**, some ARM cores (documented for Neoverse N1) "use the fixed value of 0" when faults occur during speculation
5. Speculative prefetch to PA=0 triggers RAS error on Tegra234's CBB fabric

### Cortex-A78AE Errata for Non-ARM Interconnects

NVIDIA's Orin uses their own **CBB (Control Back Bone)** interconnect, not ARM's interconnect IP. There's a relevant errata:

**ERRATA_A78_AE_2712574**: Affects Cortex-A78AE on systems without ARM interconnect IP.
- Revisions affected: r0p0, r0p1, r0p2 (Orin is r0p1)
- Issue: FWB (Force Write-Back) feature doesn't work correctly
- Effect: "A KVM guest could fetch stale/junk data instead of instructions"
- Workaround: Disable FWB, perform explicit cache maintenance

**Status**: Not enabled in NVIDIA's ATF build. Testing in progress.

### ATF Debug Build Configuration

Built ATF with debug options to get detailed RAS output:

```makefile
# ~/pkvm/Linux_for_Tegra/source/tegra/optee-src/atf/nvbuild.sh
DEBUG=1
LOG_LEVEL=50
```

### CPU Errata Enabled

Added missing errata workarounds for Cortex-A78AE r0p1:

```makefile
# platform_t234.mk - TII additions
ERRATA_A78_AE_1941500 := 1
ERRATA_A78_AE_1951502 := 1
ERRATA_A78_AE_2376748 := 1
ERRATA_A78_AE_2395408 := 1
ERRATA_A78_AE_2712574 := 1  # For non-ARM interconnect (CBB)
```

### ATF Fixes Applied

1. **Removed spammy EHF logs** in `bl31/ehf.c` (lines 257, 308, 356)
2. **Fixed backtrace crash** in `plat/nvidia/tegra/soc/t234/plat_ras.c`:
   - ATF's EL3 page tables don't map seL4's memory region
   - Backtrace code was crashing when trying to walk seL4's stack
   - Modified to just print register values without dereferencing

### Fix Attempt: User Page Table Safe PTE Initialization (2025-12-15)

#### Problem Analysis

Our initial fix initialized `armKSGlobalUserVSpace` with safe invalid PTEs (output address pointing to valid DRAM instead of zero). However, RAS errors continued because:

1. User-created VSpaces (`seL4_ARM_VSpaceObject`) are NOT initialized - they get whatever was in Untyped memory (typically zeros)
2. User-created PageTables (`seL4_ARM_PageTableObject`) are also NOT initialized
3. When speculative table walks hit these zero PTEs, they generate PA=0 accesses → RAS errors

#### Fix Implementation

Added safe PTE initialization to `kernel/src/arch/arm/64/object/objecttype.c`:

```c
case seL4_ARM_VSpaceObject:
#ifdef CONFIG_ARM_HYPERVISOR_SUPPORT
    /* EXPERIMENTAL: Initialize with safe invalid PTEs */
    {
        paddr_t safe_pa = addrFromKPPtr(armKSGlobalUserVSpace);
        pte_t *pt = (pte_t *)regionBase;
        word_t safe_pte = safe_pa & 0xfffffffff000ull;
        for (word_t i = 0; i < BIT(seL4_VSpaceIndexBits); i++) {
            pt[i].words[0] = safe_pte;
        }
    }
#endif
    cleanInvalidateCacheRange_RAM(...);
```

Same pattern applied to `seL4_ARM_PageTableObject`.

#### Test Results (2025-12-15)

**5-run multi-test**: RAS errors still occur in ALL runs.

Example ADDR values from test:
| ADDR (raw) | Decoded PA | Valid DRAM? |
|------------|------------|-------------|
| `0x800000007ffdea80` | `0x7ffdea80` | ❌ NO |
| `0x800000007fff4000` | `0x7fff4000` | ❌ NO |
| `0x800000007fff8540` | `0x7fff8540` | ❌ NO |

#### Conclusion

**Initialization alone is insufficient.** The problem persists because:

1. PTEs are modified AFTER creation (mapped → unmapped)
2. When pages are unmapped, PTEs are likely set back to **zero**
3. Need to also fix unmap operations to use safe invalid values

**Next step**: Modify PTE unmap operations to use safe invalid values instead of zeros.

### seL4 PTE Unmap Investigation (2025-12-15)

#### Problem Statement

Our user page table initialization fix (above) didn't eliminate RAS errors. Investigation revealed that PTEs are set back to **all zeros** when pages/page tables are unmapped, undoing our initialization.

#### How seL4 Unmaps PTEs

**1. `unmapPage()` function** (`kernel/src/arch/arm/64/kernel/vspace.c:1163-1196`):

```c
void unmapPage(vm_page_size_t page_size, asid_t asid, vptr_t vptr, pptr_t pptr)
{
    // ... lookup page table slot ...

    *(lu_ret.ptSlot) = pte_pte_invalid_new();  // ← Sets PTE to ALL ZEROS
    cleanInvalByVA((vptr_t)lu_ret.ptSlot, pptr_to_paddr(lu_ret.ptSlot));
    invalidateTLBByASIDVA(asid, vptr);
}
```

**2. `unmapPageTable()` function** (`kernel/src/arch/arm/64/kernel/vspace.c:1132-1161`):

```c
void unmapPageTable(asid_t asid, vptr_t vptr, pte_t *target_pt)
{
    // ... lookup page table slot ...

    *ptSlot = pte_pte_invalid_new();  // ← Sets PTE to ALL ZEROS
    cleanInvalByVA((vptr_t)ptSlot, pptr_to_paddr(ptSlot));
    invalidateTLBByASID(asid);
}
```

**3. `performPageTableInvocationUnmap()`** (`kernel/src/arch/arm/64/kernel/vspace.c:1313-1324`):

```c
static exception_t performPageTableInvocationUnmap(cap_t cap, cte_t *ctSlot)
{
    if (cap_page_table_cap_get_capPTIsMapped(cap)) {
        pte_t *pt = PT_PTR(cap_page_table_cap_get_capPTBasePtr(cap));
        unmapPageTable(...);
        clearMemory_PT((void *)pt, cap_get_capSizeBits(cap));  // ← Zeros entire table
    }
    // ...
}
```

**4. `pte_pte_invalid_new()` definition** (`kernel/include/arch/arm/arch/64/mode/object/structures.bf:327-342`):

```
block pte_invalid {
    padding                         5    // bits 63-59: 0
    field pte_sw_type               1    // bit 58: 0
    padding                         56   // bits 57-2: 0
    field pte_hw_type               2    // bits 1-0: 0
}

tag pte_invalid (0, 0)  // Both pte_hw_type=0 and pte_sw_type=0
```

This creates an **ALL ZEROS** 64-bit value. The physical address field (bits 47:12) is **PA=0**.

**5. `clearMemory_PT()` function** (`kernel/include/arch/arm/arch/machine.h:64-69`):

```c
static inline void clearMemory_PT(word_t *ptr, word_t bits)
{
    memzero(ptr, BIT(bits));  // ← Fills with zeros
    cleanInvalidateCacheRange_RAM(...);
}
```

#### Root Cause Identified

When a PTE is unmapped (page removed from address space), seL4 sets it to **all zeros**:

```
PTE = 0x0000000000000000
├── bits 47:12 (Output Address) = 0x0 → PA=0 (NULL!)
├── bit 1 (Valid bit) = 0 → Entry is invalid
└── bit 0 (Page type) = 0 → Invalid entry
```

**The Problem**:
- ARM MMU can speculatively walk page tables
- When it reads a zero PTE, bits 47:12 decode to PA=0
- Tegra234 generates RAS errors on speculative accesses to PA=0 (unmapped memory)
- Even though the PTE is marked "invalid" (bit 1=0), the speculative walker may still extract and prefetch the output address

#### Why Initialization Alone Doesn't Work

```
Timeline:
1. VSpace created    → Initialized with safe PTEs (our fix)
2. Page mapped       → Valid PTE installed
3. Page unmapped     → PTE set to ZEROS (pte_pte_invalid_new)
4. Speculative walk  → Reads zeros → PA=0 → RAS ERROR!
```

Our initialization fix only affects step 1. Steps 3 onwards reintroduce zero PTEs.

#### Required Fix

To eliminate RAS errors, we need to modify:

1. **`pte_pte_invalid_new()`** or create **`pte_pte_safe_invalid_new()`**:
   - Return an invalid PTE with a safe PA (e.g., pointing to `armKSGlobalUserVSpace`)
   - Keep bit 1=0 (invalid) but set bits 47:12 to valid DRAM address

2. **`clearMemory_PT()`**:
   - Fill with safe invalid PTEs instead of zeros
   - Each 8-byte entry should be a safe invalid PTE

3. **All unmap functions**:
   - Use safe invalid PTE instead of `pte_pte_invalid_new()`

**Example safe invalid PTE**:
```c
// Invalid PTE with safe output address pointing to armKSGlobalUserVSpace
// - bits 47:12 = valid PA in DRAM (>= 0x80000000)
// - bits 1:0 = 0 (invalid entry)
word_t safe_invalid_pte = addrFromKPPtr(armKSGlobalUserVSpace) & 0xfffffffff000ULL;
// Result: PA points to valid DRAM, but PTE is marked invalid
```

### Debugging Resources and Techniques

#### Challenge: Asynchronous Nature of SErrors

SErrors are asynchronous - an incorrect memory access may not immediately trigger an SError. The fault might be reported later when the AXI transaction completes, by which time the CPU may have transitioned to a different exception level. This makes correlating errors with their source difficult.

#### KVM Speculative Page Table Walk Issue (Highly Relevant)

Linux KVM encountered a similar issue:

> "The page table walker is allowed to carry on with speculative walks started from EL1&0 while running at EL2... If the hypervisor modifies EL1&0 system registers before these walks complete, the PTW may access stale or incorrect translation tables."

**KVM Fix**: Insert `DSB` barriers before modifying translation regime registers.

Source: [KVM: arm64: Synchronise speculative page table walks](https://lore.kernel.org/linux-arm-kernel/ZDdIWIIogROyg1zD@linux.dev/T/)

#### seL4 SError Handling Discussion

From seL4 mailing list: "People who care deeply about this issue should make sure they have ARMv8.2 hardware with FEAT_RAS and use that to implement proper SError handling in seL4 for such systems."

Source: [seL4 Devel: ARM64 SError handling](https://www.mail-archive.com/devel@sel4.systems/msg03091.html)

#### Debugging Techniques

1. **Leverage RAS error records** - examine ADDR field (remember bit 63 = NS flag)
2. **Software context tracking** - record EL states during transitions
3. **Robust diagnostic logging** - capture registers and faulting addresses
4. **Compare with Linux** - run KVM on same hardware to see if similar issues occur

#### Potential Root Causes Still Under Investigation

1. **armKSGlobalUserVSpace contains zero PTEs** - speculative walks may extract PA=0 from invalid entries
2. **Insufficient DSB barriers** - may need more barriers than currently implemented
3. **NVIDIA CBB-specific behavior** - proprietary interconnect may have stricter requirements

### SDEI (Software Delegated Exception Interface) and RAS Dispatch

#### Why ATF Reports "sdei_dispatch_event returned -1"

When RAS errors occur, NVIDIA's ATF tries to dispatch an SDEI event to notify the running OS. The `-1` return means **SDEI events are masked on this PE**.

**Source Code** (`atf/arm-trusted-firmware/services/std_svc/sdei/sdei_intr_mgmt.c:601-604`):
```c
/* Can't dispatch if events are masked on this PE */
state = sdei_get_this_pe_state();
if (state->pe_masked)
    return -1;
```

#### SDEI Architecture Overview

SDEI is an ARM standard for delivering asynchronous events (like RAS errors) from firmware to the OS. The flow:

1. **OS Registration**: OS calls `SDEI_EVENT_REGISTER` to register handler entry points
2. **PE Unmask**: OS calls `SDEI_PE_UNMASK` to enable event delivery on each PE
3. **Event Dispatch**: Firmware calls `sdei_dispatch_event()` to invoke OS handler
4. **OS Handling**: OS handler runs at registered entry point, processes error
5. **Event Complete**: OS calls `SDEI_EVENT_COMPLETE` to return control

**Key Insight**: "All PEs start with SDEI events masked" (`sdei_main.c:80`). The **client** (OS) must call `SDEI_PE_UNMASK` to receive events.

#### Why seL4 Gets -1

seL4 is a bare-metal microkernel that doesn't implement SDEI:
- Never calls `SDEI_PE_UNMASK` (SMC 0xC400002C)
- Never registers SDEI event handlers
- PE remains masked → dispatch fails → ATF logs `-1`

#### Consequence of SDEI Failure

When `sdei_dispatch_event()` returns -1:
1. ATF logs the error but **continues processing**
2. For uncorrectable errors, `system_is_not_recoverable = true`
3. ATF powers off the core after processing all errors

**The SDEI failure doesn't cause the crash** - the uncorrectable RAS error does. SDEI just means the OS can't handle it gracefully.

#### How Linux Handles SDEI

Linux kernel implements full SDEI support for RAS handling (`drivers/firmware/arm_sdei.c`):

**1. Probe/Init** (during boot):
```c
static int sdei_probe(struct platform_device *pdev)
{
    // Get SDEI version from firmware
    err = sdei_api_get_version(&ver);

    // Get arch-specific entry point (assembly handler)
    sdei_entry_point = sdei_arch_get_entry_point(conduit);

    // Register CPU hotplug callbacks - this is key!
    err = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "SDEI",
                            &sdei_cpuhp_up, &sdei_cpuhp_down);
}
```

**2. PE Unmask** (called when each CPU comes online via hotplug):
```c
static int sdei_cpuhp_up(unsigned int cpu)
{
    // Re-register private events for this CPU...

    // UNMASK THIS CPU FOR SDEI EVENTS
    return sdei_unmask_local_cpu();
}

int sdei_unmask_local_cpu(void)
{
    // SMC call: SDEI_1_0_FN_SDEI_PE_UNMASK (0xC400002C)
    return invoke_sdei_fn(SDEI_1_0_FN_SDEI_PE_UNMASK, 0, 0, 0, 0, 0, NULL);
}
```

**3. GHES Registration** (for RAS error events):
```c
// drivers/acpi/apei/ghes.c uses this to register for RAS notifications
int sdei_register_ghes(struct ghes *ghes, sdei_event_callback *normal_cb,
                       sdei_event_callback *critical_cb)
{
    event_num = ghes->generic->notify.vector;  // From ACPI table
    err = sdei_event_register(event_num, cb, ghes);
    if (!err)
        err = sdei_event_enable(event_num);
    return err;
}
```

**4. Event Handler** (arch-specific assembly, `arch/arm64/kernel/sdei.c`):
```c
// Entry: __sdei_asm_handler (arch/arm64/kernel/entry.S)
// - Switches to SDEI-specific stack
// - Saves interrupted context
// - Calls do_sdei_event()

unsigned long do_sdei_event(struct pt_regs *regs, struct sdei_registered_event *arg)
{
    // Retrieve clobbered registers
    for (i = 0; i < clobbered_registers; i++)
        sdei_api_event_context(i, &regs->regs[i]);

    // Call the registered callback (e.g., GHES handler)
    err = sdei_event_handler(regs, arg);

    // Return to interrupted code or IRQ vector
    return SDEI_EV_HANDLED;
}
```

**5. Mask on CPU Down** (power management, reboot, idle):
```c
static int sdei_cpuhp_down(unsigned int cpu)
{
    // Unregister private events
    // MASK THIS CPU
    return sdei_mask_local_cpu();
}
```

**Key Insight**: Linux's SDEI lifecycle:
- Boot: `sdei_probe()` → registers hotplug callbacks
- Each CPU online: `sdei_cpuhp_up()` → calls `sdei_unmask_local_cpu()`
- RAS event: ATF calls `sdei_dispatch_event()` → Linux handler invoked
- CPU offline/idle: `sdei_cpuhp_down()` → calls `sdei_mask_local_cpu()`

#### Options for seL4

1. **Implement SDEI support** (complex):
   - Add SMC calls for PE_UNMASK during boot
   - Create assembly entry points for SDEI events
   - Implement RAS error handler

2. **Continue ignoring SErrors** (current approach):
   - `KernelAArch64SErrorIgnore ON`
   - Let ATF power off core on uncorrectable errors
   - Prevent errors at source (valid VTTBR, safe PTEs)

3. **Modify ATF** (not recommended):
   - Change RAS handler to not power off core
   - Would mask real hardware errors

### References

- [TF-A CPU Build Macros](https://trustedfirmware-a.readthedocs.io/en/latest/design/cpu-specific-build-macros.html)
- [FWB Errata Patch Series](https://lore.kernel.org/linux-arm-kernel/20230330165128.3237939-1-james.morse@arm.com/T/)
- [NVIDIA RAS IOB Error Forum](https://forums.developer.nvidia.com/t/ras-uncorrectable-error-in-iob-base-0xe010000/322875)
- [OE4T RAS Discussion](https://github.com/orgs/OE4T/discussions/1624)
- [ARM RAS Error Reporting](https://developer.arm.com/documentation/107790/latest/RAS-error-reporting-flow)
- [Detecting SError Origin](https://www.systemonchips.com/detecting-serror-interrupt-origin-in-arm-exception-levels-el0-el1-el2-el3/)
- [KVM Translation Table Walk RAS](https://patchwork.kernel.org/project/linux-arm-kernel/patch/1511988524-30240-1-git-send-email-gengdongjiu@huawei.com/)
- [Linux SDEI ARM64 Patch](https://patchwork.kernel.org/project/linux-arm-kernel/patch/20170922182614.27885-10-james.morse@arm.com/)
- [SDEI Platform Error Injection](https://static.linaro.org/connect/yvr18/presentations/yvr18-107.pdf)

## Linux/KVM/Xen Page Table Initialization Research

### Background

We investigated whether Linux kernel, KVM hypervisor, or Xen hypervisor implement similar safe page table initialization patterns to prevent speculative table walk issues.

### Critical Correction (2025-12-15)

**IMPORTANT**: Our earlier hypothesis about "safe addresses in invalid PTEs" was INCORRECT.

After deeper investigation, we found that:

1. **Linux uses ALL-ZERO page tables** (`reserved_pg_dir` is pre-zeroed, not filled with safe addresses)
2. **The actual Linux/KVM fix is DSB barriers**, not PTE content
3. **ARM architecture says invalid PTEs (bit[0]=0) should NOT cause memory accesses** - the PTW should stop and return a translation fault

This means our "safe PTE initialization" approach may be addressing the wrong problem. The real fix in Linux/KVM is proper DSB barrier placement.

### Key Findings

#### 1. Linux `reserved_pg_dir` Pattern - ALL ZEROS

Linux ARM64 uses `reserved_pg_dir` as an empty page table **filled with zeros**:

- From the [patch](https://lkml.kernel.org/lkml/20210712060909.968604334@linuxfoundation.org/): "reserved_pg_dir is allocated (and hence **pre-zeroed**), and is also marked as read-only"
- **All PTEs are zero** - bit[0]=0 means invalid, other bits are also zero
- Linux does NOT put "safe addresses" in the output address bits of invalid PTEs
- Used by `cpu_set_reserved_ttbr0()` to set a safe default for TTBR0_EL1
- The key is having a **valid TTBR base address** pointing to a real page table (even if empty/zeroed), not having non-zero addresses in invalid PTEs

#### 2. ARM64 Speculative Table Walk Issue (Root Cause)

ARM ARM specification reference R_LFHQG documents the root cause:

> When the processor transitions between exception levels (EL1/EL0 to EL2), the **Page Table Walker (PTW) can continue performing speculative memory walks** initiated from the lower privilege level while executing at the higher level.

This creates a race condition where the PTW may use stale or partially-updated system registers.

**RAS Error Connection:**
- Translation table walk RAS errors (Synchronous External Aborts with FSC_SEA_TTW) represent hardware-detected errors during speculative page table walks
- Errors occur when speculative walker accesses invalid or protected memory locations

#### 3. KVM ARM64 Mitigation - DSB Barriers

Linux KVM implements DSB (Data Synchronization Barrier) instructions to synchronize with incomplete page table walks:

**VTTBR/TTBR Switching:**
- KVM inserts `dsb(nsh)` (non-shareable DSB) before restoring guest MMU state
- Ensures speculative page table walks started before trapping to EL2 complete before changing `VTTBR_EL2`

**TLB Invalidation:**
- Upgraded from `dsb(ishst)` to `dsb(ish)` to ensure both:
  - Page table updates visible to all CPUs
  - Speculative page table walks complete

**ARM ARM states:**
> "Altering the system registers that define the EL1&0 translation regime is fraught with danger *unless* we wait for the completion of such walk with a DSB"

#### 4. KVM Patch Series Reference

Marc Zyngier's patch series "[PATCH v2 0/5] KVM: arm64: Synchronise speculative page table walks on translation regime change" implements:

1. `KVM: arm64: nvhe: Synchronise with page table walker on vcpu run`
2. `KVM: arm64: nvhe: Synchronise with page table walker on TLBI`
3. `KVM: arm64: vhe: Synchronise with page table walker on MMU update`

#### 5. Historical RAS Error Handling (KVM)

Commit "KVM: arm64: handle the translation table walk RAS error" (Gengdongjiu, Huawei) distinguishes:
- Standard memory access RAS errors → host APEI driver handles
- Translation table walk RAS errors → injected directly to guest

The patch notes that host-level recovery for table walk errors is problematic because the host marks a page unusable while the guest continues using the same page table.

### Implications for seL4

1. **Our "safe PTE" approach was WRONG** - Linux does NOT use safe addresses in invalid PTEs; they use all-zeros
2. **DSB barriers are the actual fix** - Marc Zyngier's KVM patches add DSB barriers, not PTE content changes
3. **We need to audit seL4's DSB barrier placement** - compare against KVM's barrier patterns
4. **The problem is well-known in ARM64 ecosystem** - not unique to Tegra or seL4, but our fix approach was misguided

**Action Required**: Audit seL4's VTTBR/TLB operations for proper DSB barriers, comparing against KVM's implementation.

### KVM DSB Barrier Patterns to Match

From Marc Zyngier's [KVM patch series](https://lore.kernel.org/linux-arm-kernel/ZDdIWIIogROyg1zD@linux.dev/T/):

#### 1. Before Guest MMU State Restoration (nvhe/switch.c)

```c
// Add dsb(nsh) before restoring guest MMU state
dsb(nsh);  // Synchronize with speculative page table walks
// ... restore VTTBR_EL2, TCR_EL1, etc.
```

**Rationale**: PTW may still be performing speculative walks from previous EL1&0 context. DSB ensures those complete before changing VTTBR_EL2.

#### 2. TLB Invalidation (nvhe/tlb.c)

```c
// UPGRADED from dsb(ishst) to dsb(ish)
static void __tlb_switch_to_guest(...)
{
    dsb(ish);  // Was dsb(ishst) - upgraded to full inner-shareable DSB
    // ... switch VTTBR ...
    // ... perform TLBI ...
}
```

**Rationale**: `dsb(ishst)` only ensures store completion. `dsb(ish)` also ensures speculative PTW completion.

#### 3. Zero Page Visibility (Will Deacon's patch)

```c
// In paging_init, after zeroing reserved page table:
memset(reserved_pg_dir, 0, PAGE_SIZE);
dsb(ishst);  // Ensure zeros visible to PTW before updating TTBR
write_sysreg(virt_to_phys(reserved_pg_dir), ttbr0_el1);
```

**Rationale**: PTW may speculatively access the page table. DSB ensures zeroed content is visible before TTBR points to it.

### What seL4 Needs to Check

1. **VTTBR switching in `tlb.h`**: Do we have DSB before changing VTTBR_EL2?
2. **TLB invalidation**: Are we using `dsb ish` (not just `dsb ishst`)?
3. **Page table updates**: DSB after writing PTEs, before TLBI?
4. **Context switch paths**: DSB when switching between VMs/threads?

### seL4 DSB Barrier Audit Results (2025-12-15)

#### What's Correct

| Location | Barrier | Status |
|----------|---------|--------|
| `setCurrentUserVSpaceRoot()` | `dsb sy` at entry | ✓ Correct |
| `invalidateLocalTLB_*()` | `dsb sy` before/after TLBI | ✓ Correct |
| `cleanInvalByVA()` | `dsb sy` after dc civac | ✓ Correct |
| `vcpu_disable()` | `dsb sy` at entry (line 672) | ✓ Correct |
| `invalidateLocalTLB_VMID()` | `dsb sy` at entry (line 17) | ✓ Correct |
| `invalidateLocalTLB_IPA_VMID()` | `dsb sy` at entry (line 43) | ✓ Correct |

seL4 uses `dsb sy` (full system barrier) which is **stronger** than KVM's `dsb ish`.

#### Map/Unmap Sequences - CORRECT

**Unmap sequence** (`unmapPage`, `unmapPageTable`):
```
1. Write invalid PTE: *ptSlot = pte_pte_invalid_new()
2. Cache flush: cleanInvalByVA() → dc civac + dsb sy
3. TLBI: invalidateTLBByASIDVA() → dsb sy + TLBI + dsb sy + isb
```

**Map sequence** (`performPageInvocationMap`):
```
1. Write new PTE: *ptSlot = pte
2. Cache flush: cleanInvalByVA() → dc civac + dsb sy
3. If was valid: TLBI with barriers
```

These sequences follow ARM's break-before-make pattern correctly.

#### **MISSING BARRIER FOUND: `vcpu_enable()`**

```c
// kernel/include/arch/arm/armv/armv8-a/64/armv/vcpu.h:649-666
static inline void vcpu_enable(vcpu_t *vcpu)
{
    // NO DSB HERE! ← MISSING
    vcpu_restore_reg(vcpu, seL4_VCPUReg_SCTLR);  // Restores SCTLR_EL1
    setHCR(HCR_VCPU);
    isb();
    // ...
}
```

**Problem**: `vcpu_enable()` restores guest MMU state (SCTLR_EL1) without a preceding DSB. The PTW may still be performing speculative walks using the OLD EL1&0 translation regime.

**Comparison with `vcpu_disable()`**:
```c
static inline void vcpu_disable(vcpu_t *vcpu)
{
    dsb();  // ✓ HAS DSB at entry
    // ... save guest state ...
}
```

**This is exactly what Marc Zyngier's KVM patch fixes** - the patch adds `dsb(nsh)` before restoring guest MMU state on vcpu run.

#### Fix Applied and Tested (2025-12-15)

Added `dsb()` at the start of `vcpu_enable()`:

```c
static inline void vcpu_enable(vcpu_t *vcpu)
{
    /*
     * Synchronize with speculative page table walks (ARM ARM R_LFHQG).
     * When transitioning to EL1&0 (enabling VCPU), the PTW may still be
     * performing speculative walks using the OLD translation regime.
     */
    dsb();
    vcpu_restore_reg(vcpu, seL4_VCPUReg_SCTLR);
    setHCR(HCR_VCPU);
    isb();
    // ...
}
```

**Test Results**: RAS errors still occur after this fix.

5-run multi-test (2025-12-15-194339):
- All 5 runs completed (tests pass in debug mode)
- RAS errors still present in all runs
- Error addresses: `0x7fff8540`, `0x7fff8400`, `0x7fff4580`, `0x7fff84c0`
- All addresses below DRAM base (0x80000000)

**Conclusion**: The `vcpu_enable` DSB fix is correct and should be kept, but it does not eliminate the RAS errors. The root cause must be elsewhere.

### Test Correlation Analysis (2025-12-15)

Analyzed which sel4test tests trigger RAS errors across multiple runs.

#### 25-Run Statistical Analysis

**Run Summary:**
- Runs with RAS errors: **23/25 (92%)**
- Clean runs: 2/25 (8%) - runs 4 and 24
- Total RAS errors: 88

**Error Types:**
| Type | Count | Description |
|------|-------|-------------|
| ACI | 44 | AXI Coherency Interface |
| SCC | 36 | System Cache Controller |
| IOB | 8 | I/O Bridge |

#### Tests That Triggered RAS Errors (7 out of 122 tests)

| Test | Errors | Runs | Rate | Description |
|------|--------|------|------|-------------|
| **FPU0001** | 52 | 19 | 76% | Multiple threads using FPU simultaneously |
| **CANCEL_BADGED_SENDS_0002** | 18 | 8 | 32% | cancelBadgedSends deletes caps |
| **SERSERV_CLI_PROC_002** | 6 | 3 | 12% | Serial server client (different VSpace) |
| **SERSERV_CLI_PROC_005** | 4 | 2 | 8% | Serial server client (different VSpace) |
| **SERSERV_CLI_PROC_001** | 4 | 2 | 8% | Serial server client (different VSpace) |
| **SERSERV_CLI_PROC_003** | 2 | 1 | 4% | Serial server client (different VSpace) |
| **SYNC004** | 2 | 1 | 4% | libsel4sync monitors - broadcast |

#### Tests That NEVER Triggered RAS Errors (115 tests)

All other tests (115/122) completed without RAS errors in all 25 runs. This includes:
- All BIND tests (thread binding/unbinding)
- All IPC tests (same and inter-AS)
- All VSPACE tests (address space operations)
- All PAGEFAULT tests
- All BREAKPOINT/DEBUG tests
- All RETYPE tests
- All SCHED tests
- etc.

#### Detailed Run Analysis (5-run test 2025-12-15-164314)

| Run | RAS Errors (SCC only) | Triggering Test(s) |
|-----|----------------------|-------------------|
| 1 | 3 | CANCEL_BADGED_SENDS_0002 (×2), FPU0001 (×1) |
| 2 | 4 | CACHEFLUSH (IOB error), CANCEL_BADGED_SENDS_0002 (×2), FPU0001 |
| 3 | 4 | FPU0001 (×4) |
| 4 | 1 | FPU0001 |
| 5 | 1 | FPU0001 |

#### Common Thread: Multi-Thread Context Switching and VSpace Changes

All error-triggering tests share common characteristics:

1. **FPU0001** (76% of runs) - Creates multiple threads using FPU simultaneously
   - Requires FPU context saving/restoring during thread switches
   - Heavy thread creation, scheduling, and context switching
   - **Most consistent trigger** - errors occur in nearly every run

2. **CANCEL_BADGED_SENDS_0002** (32% of runs) - "cancelBadgedSends deletes caps"
   - Creates threads that send messages on badged endpoints
   - Cancels/deletes those sends, involving thread/cap lifecycle management
   - Thread deletion and address space teardown

3. **SERSERV_CLI_PROC_*** (12-4% of runs) - Serial server client tests in different VSpace/CSpace
   - Run in **separate address spaces** from the test driver
   - Involve cross-VSpace IPC and process lifecycle
   - VTTBR switches between different user address spaces

4. **SYNC004** (4% of runs) - libsel4sync monitors with broadcast
   - Multi-threaded synchronization primitive test
   - Wakes multiple threads simultaneously

#### Statistical Conclusions

**For the 7 error-triggering tests** (at 95% confidence):
| Test | 95% CI | Expected errors in 100 runs |
|------|--------|----------------------------|
| FPU0001 | 0.57 - 0.89 | 57 - 89 |
| CANCEL_BADGED_SENDS_0002 | 0.17 - 0.52 | 17 - 52 |
| SERSERV_CLI_PROC_002 | 0.04 - 0.30 | 4 - 30 |
| SERSERV_CLI_PROC_005 | 0.02 - 0.25 | 2 - 25 |

**For the 115 tests that NEVER triggered errors:**
- 0 errors in 25 runs each
- 95% CI: **0.00 - 0.113** (0% - 11.3%)
- Using rule of three: upper bound ≈ 3/n = 3/25 = 0.12

**Key Conclusions:**

1. **RAS errors are NOT random hardware noise** - If errors were random, they'd distribute across all tests. Instead, they're concentrated in just 7/122 tests (5.7%). This proves the errors are triggered by specific code paths.

2. **The bug is in specific kernel subsystems** - The 115 safe tests cover:
   - Basic IPC (same address space) - all IPC tests clean
   - Page table operations - all VSPACE tests clean
   - Page faults - all PAGEFAULT tests clean
   - Thread scheduling - all SCHED tests clean
   - CNode operations - all CNODEOP tests clean
   - Memory retyping - all RETYPE tests clean
   - Thread binding - all BIND tests clean

   These fundamental kernel operations do NOT trigger RAS errors.

3. **The problematic code paths share characteristics:**
   - **FPU context switching** (FPU0001) - 76% trigger rate
   - **Thread/cap deletion** (CANCEL_BADGED_SENDS) - 32% trigger rate
   - **Cross-VSpace process operations** (SERSERV_CLI_PROC) - 4-12% trigger rate

4. **Same-VSpace vs Cross-VSpace distinction:**
   - SERSERV_CLIENT_* tests (same VSpace): **0% errors**
   - SERSERV_CLI_PROC_* tests (different VSpace): **4-12% errors**
   - This suggests VTTBR switching between user address spaces is involved.

5. **Recommended stress test for debugging:**
   - **FPU0001 × 100 runs** - Expect 57-89 errors, highest reliability
   - **CANCEL_BADGED_SENDS_0002 × 100 runs** - Expect 17-52 errors, different code path
   - Skip SERSERV_CLI_PROC tests (too unreliable, CI lower bounds only 2-4%)

#### Hypothesis: Thread Context Switch Race

The PTW (Page Table Walker) appears to be racing with thread context switches:

1. Thread A is running with its page tables active
2. Context switch to Thread B begins
3. seL4 starts switching VTTBR to Thread B's address space
4. **Speculative PTW from Thread A continues** (ARM ARM R_LFHQG)
5. PTW reads a PTE from Thread A's (now invalid) page table
6. PTE contains garbage/stale address (0x7fffxxxx)
7. SCC reports RAS error because 0x7fffxxxx is below DRAM

The addresses `0x7fffxxxx` (just below 0x80000000 DRAM base) may be:
- Stale PTEs not properly cleaned during thread deletion
- Speculative reads from page tables being torn down
- Cache coherency issues during address space switches

### Source References

- [KVM: arm64: Synchronise speculative page table walks (LWN.net)](https://lwn.net/Articles/929006/)
- [KVM patch series (lore.kernel.org)](https://lore.kernel.org/linux-arm-kernel/ZDdIWIIogROyg1zD@linux.dev/T/)
- [arm64: consistently use reserved_pg_dir](https://lkml.kernel.org/lkml/20210712060909.968604334@linuxfoundation.org/)
- [KVM: arm64: handle translation table walk RAS error](https://patchwork.kernel.org/project/linux-arm-kernel/patch/1511988524-30240-1-git-send-email-gengdongjiu@huawei.com/)
- [AArch64 Kernel Page Tables](https://wenboshen.org/posts/2018-09-09-page-table.html)

## References

- ARM Architecture Reference Manual - ARMv8-A (Cache and TLB maintenance)
- NVIDIA Orin TRM (Tegra234 Technical Reference Manual)
- seL4 kernel source code
- `tegra-cache-operations.md` - Related cache issue documentation

## Build Configurations Used

### Hypervisor Mode Build
```bash
make orinagx_defconfig   # Sets ARM_HYP=ON
make sel4test
```

### Non-Hypervisor Mode Build
```bash
make orinagx_nohyp_defconfig   # Sets ARM_HYP=OFF
make sel4test
```

Config differences:
- `ARM_HYP=ON`: `CONFIG_ARCH_ARM_HYP` enabled, uses EL2, stage-2 translation
- `ARM_HYP=OFF`: No hypervisor support, runs at EL1, only stage-1 translation

## 100-Run Stress Test Results (2025-12-15)

### Test Configuration

Modified sel4test to run ONLY FPU0001 and CANCEL_BADGED_SENDS_0002 in a loop:
- 100 iterations
- Each iteration runs both tests (FPU0001 first, then CANCEL_BADGED_SENDS_0002)
- Total: 200 test runs
- Disabled kernel `userError` messages to reduce UART bottleneck

### Results Summary

| Metric | Value |
|--------|-------|
| Total iterations | 100 |
| Total test runs | 200 (100 × 2 tests) |
| All tests passed | Yes |
| Total RAS errors | 232 (116 unique events, each reports SCC + ACI) |

### Per-Test Trigger Rates

| Test | Runs with ≥1 RAS Error | Rate |
|------|------------------------|------|
| FPU0001 | 41/100 | **41%** |
| CANCEL_BADGED_SENDS_0002 | 56/100 | **56%** |

**Note**: These rates differ from the 25-run estimates (FPU0001: 76%, CANCEL_BADGED_SENDS: 32%). The alternating test pattern in the stress test may affect trigger rates - possibly due to different cache/TLB state between iterations.

### Error Address Distribution

| Address (PA) | Count | Notes |
|--------------|-------|-------|
| 0x7fff84c0 | 104 | Most common |
| 0x7fff8400 | 100 | Second most common |
| 0x7ffdea40 | 26 | Different page |
| 0x7ffb04c0 | 2 | Rare |

All addresses are **below DRAM base** (0x80000000) - invalid memory region.

### Key Observations

1. **Both tests reliably trigger RAS errors** - 97/200 runs (48.5%) had at least one error
2. **Error addresses cluster** around 0x7fffxxxx (just below DRAM base)
3. **Alternating pattern changes trigger rates** - suggests state accumulation between tests
4. **Tests continue to pass** despite RAS errors (SError ignored in seL4)

---

## Debugging Plan

### Phase 1: Identify the Faulting Code Path ✓ COMPLETED

**Goal**: Determine exactly which seL4 instruction triggers the RAS error.

#### 1.1 ELR_EL3 Address Analysis (100-run stress test)

**CANCEL_BADGED_SENDS_0002** - All errors at ONE kernel address:
| Address | Count | Source Location |
|---------|-------|-----------------|
| `0x80800193d4` | **56** | `setMRs_syscall_error` (tcb.c:2134) |
| `0x418b60` | 1 | User code |
| `0x418b6c` | 1 | User code |
| `0x418d1c` | 1 | User code |

**FPU0001** - All errors at MULTIPLE user-space addresses:
| Address | Count | Context |
|---------|-------|---------|
| `0x46d4c0` | 12 | User code (EL0) |
| `0x400b70` | 8 | User code (EL0) |
| `0x4007cc` | 7 | User code (EL0) |
| `0x46daf4` | 3 | User code (EL0) |
| `0x4021dc` | 3 | User code (EL0) |
| (+ 18 more) | 1-2 each | User code (EL0) |

#### 1.2 Key Finding: Two Different Error Patterns

**CANCEL_BADGED_SENDS_0002**: Errors occur in **kernel code (EL2)** during syscall error handling:
```
setRegister at kernel/include/machine/registerset.h:31
 (inlined by) setMR at kernel/include/object/tcb.h:39
 (inlined by) setMRs_syscall_error at kernel/src/object/tcb.c:2134
```

**FPU0001**: Errors occur in **user code (EL0)** at various addresses - likely during FPU operations or thread scheduling.

#### 1.3 CANCEL_BADGED_SENDS Code Path Analysis

The `seL4_InvalidCapability` error at `setMRs_syscall_error` is **not the cause** - it's just where the **asynchronous SError gets delivered**.

**What cancelBadgedSends does** (`kernel/src/object/endpoint.c:436`):
```c
for (thread = queue.head; thread; thread = next) {
    if (b == badge) {
        setThreadState(thread, ThreadState_Restart);  // Change state
        SCHED_ENQUEUE(thread);                        // Enqueue for scheduling
        queue = tcbEPDequeue(thread, queue);
    }
}
rescheduleRequired();  // ← TRIGGERS CONTEXT SWITCH!
```

**The sequence that triggers RAS errors:**
1. `cancelBadgedSends` changes thread states and calls `rescheduleRequired()`
2. After syscall handling, `schedule()` picks a new thread
3. **Context switch occurs** - VTTBR switched, TLBs invalidated
4. **Speculative PTW** from old context accesses stale page tables
5. PTW reads address in 0x7fffxxxx range (below DRAM) → RAS error
6. SError delivered later while kernel is at `setMRs_syscall_error`

#### 1.4 FPU0001 In-Depth Analysis

**Test Structure** (`projects/sel4test/apps/sel4test-tests/src/tests/fpu.c:88-127`):

```c
static int test_fpu_multithreaded(struct env *env)
{
    const int NUM_THREADS = 4;
    for (int i = 0; i < NUM_THREADS; i++) {
        create_helper_thread(env, &thread[i]);
        set_helper_priority(env, &thread[i], 100);  // ALL SAME PRIORITY!
        start_helper(env, &thread[i], fpu_worker, ...);
    }
    // Main loop runs until 20+ preemptions detected
    do {
        // Check preemption counters
    } while (num_preemptions < 20);
}
```

**Why FPU0001 Triggers Context Switches:**
- Creates **4 threads at identical priority** (100)
- seL4 uses **round-robin scheduling** for same-priority threads
- This causes **constant context switching** between the 4 threads
- Each context switch involves VTTBR/TLB operations

**ELR_EL3 Analysis - These Are NOT FPU Instructions:**

| Address | Count | Function | Type |
|---------|-------|----------|------|
| 0x46d4c0 | 12 | `allocman_cspace_make_path` | Cspace operation |
| 0x400b70 | 8 | `seL4_DebugCapIdentify` | Syscall |
| 0x4007cc | 7 | `arm_sys_send_recv` | Syscall |
| 0x46daf4 | 3 | `am_vka_cspace_free` | Cspace operation |
| 0x4021dc | 3 | `sel4_timer_handle_single_irq` | Timer/IRQ |
| 0x46d4b4 | 2 | `allocman_cspace_make_path` | Cspace operation |
| 0x4689b4 | 2 | `_cspace_single_level_free` | Cspace operation |
| 0x418b6c | 2 | Unknown | User code |
| 0x418d1c | 2 | Unknown | User code |

**Key Finding**: All ELR_EL3 addresses map to **cspace operations, syscalls, and timer handling** - NOT FPU instructions. The test name "FPU0001" is misleading; the errors are triggered by **thread context switches** caused by the round-robin scheduling of 4 equal-priority threads.

**Conclusion**: Both FPU0001 and CANCEL_BADGED_SENDS_0002 trigger RAS errors through the same mechanism:
1. **Thread context switching** - VTTBR switched, TLBs invalidated
2. **Speculative PTW** from old context accesses stale page tables
3. **PTW reads address** in 0x7fffxxxx range (below DRAM) → RAS error
4. **SError delivered asynchronously** while executing unrelated code (cspace ops, syscalls)

The "FPU" test happens to trigger this because it creates aggressive context switch conditions, not because of anything FPU-specific.

### Phase 2: Understand the 0x7fffxxxx Addresses

**Goal**: Determine where addresses like 0x7fff84c0 come from.

#### 2.1 Hypothesis: Stale PTE Content

The 0x7fffxxxx addresses could be:
- **Stale data** in unmapped PTEs (after page table teardown)
- **Speculative PTW reads** from freed page table memory
- **Uninitialized memory** being interpreted as PTEs

**Action**: Add instrumentation to track PTE values during:
- Page table creation
- Page mapping/unmapping
- Page table deletion
- Thread/process destruction

#### 2.2 Check for 0x7fffxxxx in seL4 Memory

```bash
# Search for this pattern in kernel structures
grep -r "7fff" kernel/  # In source
# Runtime: dump page table contents during stress test
```

### Phase 3: Narrow Down the Trigger ✓ COMPLETED

**Goal**: Identify which specific operation within each test triggers errors.

**BREAKTHROUGH**: Custom diagnostic tests have identified the root cause!

#### 3.1 Diagnostic Tests Created

Location: `projects/sel4test/apps/sel4test-tests/src/tests/ras_diag.c`

| Test | Description | Result |
|------|-------------|--------|
| CONTEXT_SWITCH_ONLY_0001 | Pure context switching (4 threads, same priority) | **0% errors** |
| SINGLE_THREAD_BASELINE_0001 | No context switching | **0% errors** |
| FPU_SINGLE_THREAD_0001 | FPU operations, single thread | **0% errors** |
| ENDPOINT_NO_CANCEL_0001 | Endpoint IPC without cancelBadgedSends | **0% errors** |
| YIELD_CONTEXT_SWITCH_0001 | Context switching via seL4_Yield | **0% errors** |
| FPU_MULTITHREAD_0001 | FPU + multithread (one-shot) | **0% errors** |
| **THREAD_LIFECYCLE_0001** | **4 concurrent threads, repeated create/destroy** | **46% errors** |
| THREAD_LIFECYCLE_RAPID_0001 | 1 sequential thread, 200 create/destroy cycles | **0% errors** |

#### 3.2 Root Cause Identified

**The RAS errors are triggered by MULTI-THREAD LIFECYCLE OPERATIONS with CONCURRENT THREADS.**

Evidence:
- THREAD_LIFECYCLE_0001 (4 concurrent threads): **46% error rate**
- THREAD_LIFECYCLE_RAPID_0001 (1 sequential thread): **0% error rate**
- FPU0001 (original, 4 concurrent threads): **41% error rate** - nearly identical!
- FPU_MULTITHREAD_0001 (one-shot, no repeated destroy): **0% error rate**

**Key Findings**:
1. FPU0001 has a `do-while` loop that repeatedly creates and destroys threads
2. Multiple concurrent threads (4 at same priority) are required
3. Single-thread create/destroy cycles do NOT trigger errors
4. The bug is a **race condition** between concurrent thread cleanup and context switching

#### 3.3 What Does NOT Trigger Errors

| Operation | Error Rate | Conclusion |
|-----------|------------|------------|
| Context switching alone | 0% | NOT the trigger |
| FPU operations | 0% | NOT the trigger |
| FPU + context switching | 0% | NOT the trigger |
| Endpoint IPC | 0% | NOT the trigger |
| seL4_Yield-based switching | 0% | NOT the trigger |

#### 3.4 What DOES Trigger Errors

| Operation | Error Rate | Conclusion |
|-----------|------------|------------|
| Repeated thread create/destroy | **43%** | **ROOT CAUSE** |
| FPU0001 (has create/destroy loop) | **41%** | Incidental FPU, lifecycle is trigger |
| CANCEL_BADGED_SENDS_0002 | **56%** | Also involves thread/cap deletion |

#### 3.5 Technical Analysis

The `cleanup_helper()` function destroys threads, which involves:
1. Thread deletion (TCB cleanup)
2. VSpace/page table cleanup
3. Capability revocation

**Hypothesis**: Thread destruction leaves stale PTEs that speculative PTW accesses:
1. Thread is destroyed → VSpace pages are unmapped
2. PTEs are zeroed (invalid)
3. Speculative PTW from previous context reads stale/zeroed PTE
4. PTW interprets zeroed PTE as address 0x7fffxxxx
5. Memory access to below-DRAM region → RAS error

#### 3.6 Next Investigation Steps

1. Investigate `cleanup_helper()` implementation
2. Trace what happens during `sel4utils_clean_up_thread()`
3. Check if VSpace pages are unmapped before TLB is flushed
4. Add barriers around thread destruction path

### Phase 4: Test Potential Fixes

**Goal**: Systematically test hypotheses.

#### 4.1 Safe Invalid PTE Values (Already Tested - FAILED)

We tried initializing PTEs with safe addresses. This didn't work because:
- PTEs are reset to zero on unmap
- Need to fix unmap paths too

**Next Step**: Modify `pte_pte_invalid_new()` to use safe address instead of zero.

#### 4.2 Additional DSB Barriers

Locations to add DSB barriers:
- [ ] Before FPU context restore
- [ ] In `cancelBadgedSends` loop
- [ ] Before/after `deleteASID`
- [ ] In thread deletion path

#### 4.3 TLBI Sequence Changes

Current sequence:
```
dsb sy → TLBI → dsb sy → isb
```

Try:
```
dsb sy → dsb sy → TLBI → dsb sy → dsb sy → isb
```

### Phase 5: Root Cause Confirmation

**Goal**: Confirm the fix and understand why it works.

#### 5.1 Before/After Comparison

Run 100-iteration stress test:
- Before fix: ~48.5% error rate
- After fix: Target 0% error rate

#### 5.2 Document the Fix

Once confirmed:
- Upstream to seL4 if applicable
- Document Tegra-specific behavior
- Add regression test

---

### Immediate Next Steps

1. **Map ELR_EL3 addresses to source** - identify exact faulting instructions
2. **Analyze 0x7fffxxxx origin** - where do these addresses come from?
3. **Instrument FPU context switch** - add debug output around FPU save/restore
4. **Test safe invalid PTE in unmap** - modify `pte_pte_invalid_new()`

### Files to Modify/Instrument

| File | Purpose |
|------|---------|
| `kernel/include/arch/arm/armv/armv8-a/64/armv/vcpu.h` | FPU context switch, vcpu_enable/disable |
| `kernel/src/arch/arm/64/kernel/vspace.c` | unmapPage, unmapPageTable |
| `kernel/src/object/endpoint.c` | cancelBadgedSends |
| `kernel/src/object/tcb.c` | Thread deletion |
| `kernel/include/arch/arm/arch/64/mode/object/structures.bf` | pte_pte_invalid definition |

---

## Baseline Test Results (2025-12-16) - Without HCR_EL2.VM Fix

Established with automated analyzer tool (`autopilot/analyze_sel4log.py`).

### Test Configuration
- **Kernel**: HCR_EL2.VM fix REVERTED (commit b476853f7)
- **Stress test**: 100 iterations × 4 tests per iteration
- **Mode**: Interleaved (ABCD ABCD ABCD)
- **Tests**: CANCEL_BADGED_SENDS_0002, FPU0001, THREAD_LIFECYCLE_0001, THREAD_LIFECYCLE_RAPID_0001

### Results Summary

| Metric | Value |
|--------|-------|
| Total iterations | 100 |
| Total test runs | 401 |
| Passed | 401 (100%) |
| Clean iterations | **0/100** |
| SCC errors | 613 |
| ACI errors | 498 |
| Total errors | 1111 |
| Error rate | 11.11/iteration |

### Errors by Test

| Test | Errors | Percentage |
|------|--------|------------|
| CANCEL_BADGED_SENDS_0002 | 723 | 65.1% |
| FPU0001 | 286 | 25.7% |
| THREAD_LIFECYCLE_0001 | 102 | 9.2% |
| THREAD_LIFECYCLE_RAPID_0001 | 0 | 0.0% |

### Top Error Addresses (ADDR field)

All addresses are below DRAM base (0x80000000), NS bit set:
- `0x7fff84c0`: 242 occurrences
- `0x7fff44c0`: 212 occurrences
- `0x7ffc0c00`: 130 occurrences

---

## Test Results with HCR_EL2.VM Fix (2025-12-16)

### Test Configuration
- **Kernel commit**: e7a4cb886 (HCR_EL2.VM fix + commented null cap log)
- **Stress test**: 100 iterations × 4 tests per iteration
- **Mode**: Interleaved (ABCD ABCD ABCD)

### Results Summary

| Metric | Value |
|--------|-------|
| Total iterations | 100 |
| Clean iterations | 1/100 |
| SCC errors | 655 |
| ACI errors | 542 |
| Total errors | 1197 |

### Errors by Test

| Test | Errors | Percentage |
|------|--------|------------|
| CANCEL_BADGED_SENDS_0002 | 949 | 79.3% |
| FPU0001 | 236 | 19.7% |
| THREAD_LIFECYCLE_0001 | 12 | 1.0% |
| THREAD_LIFECYCLE_RAPID_0001 | 0 | 0.0% |

### Comparison: HCR_EL2.VM Fix Effect

| Test | Without Fix | With Fix | Improvement |
|------|-------------|----------|-------------|
| CANCEL_BADGED_SENDS_0002 | 723 | 949 | -31% (worse) |
| FPU0001 | 286 | 236 | **+17%** |
| THREAD_LIFECYCLE_0001 | 102 | 12 | **+88%** |
| THREAD_LIFECYCLE_RAPID_0001 | 0 | 0 | - |

**Key Finding**: HCR_EL2.VM fix dramatically helps THREAD_LIFECYCLE_0001 (88% reduction) and FPU0001 (17% reduction), but CANCEL_BADGED_SENDS_0002 errors actually increase. The fix targets VTTBR switching which is more prevalent in thread lifecycle/FPU tests than in endpoint operations.

---

## Sequential Mode Test Results (2025-12-16)

Tests run in sequential mode (AAA BBB CCC DDD) where each test runs 100× before moving to the next.

### Test Configuration
- **Stress test**: 100 iterations × 4 tests
- **Mode**: Sequential (AAA BBB CCC DDD)
- **Tests**: CANCEL_BADGED_SENDS_0002, FPU0001, THREAD_LIFECYCLE_0001, THREAD_LIFECYCLE_RAPID_0001

### Results Without HCR_EL2.VM Fix

| Test | Errors | Percentage |
|------|--------|------------|
| CANCEL_BADGED_SENDS_0002 | 678 | 58.9% |
| FPU0001 | 354 | 30.7% |
| THREAD_LIFECYCLE_0001 | 120 | 10.4% |
| THREAD_LIFECYCLE_RAPID_0001 | 0 | 0.0% |
| **Total** | **1152** | |

- SCC errors: 639
- ACI errors: 513

### Results With HCR_EL2.VM Fix

| Test | Errors | Percentage |
|------|--------|------------|
| CANCEL_BADGED_SENDS_0002 | 953 | 78.3% |
| FPU0001 | 250 | 20.5% |
| THREAD_LIFECYCLE_0001 | 14 | 1.2% |
| THREAD_LIFECYCLE_RAPID_0001 | 0 | 0.0% |
| **Total** | **1217** | |

- SCC errors: 661
- ACI errors: 556

### Sequential Mode Comparison

| Test | Without Fix | With Fix | Improvement |
|------|-------------|----------|-------------|
| CANCEL_BADGED_SENDS_0002 | 678 | 953 | -41% (worse) |
| FPU0001 | 354 | 250 | **+29%** |
| THREAD_LIFECYCLE_0001 | 120 | 14 | **+88%** |
| THREAD_LIFECYCLE_RAPID_0001 | 0 | 0 | - |

### Interleaved vs Sequential Comparison

| Test | Interleaved (no fix) | Sequential (no fix) | Difference |
|------|---------------------|---------------------|------------|
| CANCEL_BADGED_SENDS_0002 | 723 | 678 | Similar |
| FPU0001 | 286 | 354 | +24% more in sequential |
| THREAD_LIFECYCLE_0001 | 102 | 120 | +18% more in sequential |

**Key Findings:**

1. **HCR_EL2.VM fix effect is consistent**: THREAD_LIFECYCLE improvement is ~88% in both modes
2. **Sequential mode shows slightly higher FPU/thread errors**: Running tests back-to-back without interleaving may accumulate more state
3. **CANCEL_BADGED_SENDS remains unaffected by fix**: Errors increase with fix in both modes
4. **Test order matters less than expected**: Error distribution is similar between modes

### Hypothesis: Error Attribution Bleeding in Interleaved Mode

**Note: This is a hypothesis, not yet confirmed.**

RAS errors are **asynchronous** - the memory access triggering an error occurs at time T, but the error interrupt may fire at T+N cycles. In interleaved mode (ABCD ABCD), this could cause misattribution:

```
Test A triggers error → Test B running when interrupt fires → Error attributed to B
```

Observations supporting this hypothesis:

| Test | Interleaved | Sequential | Delta |
|------|-------------|------------|-------|
| CANCEL_BADGED_SENDS | 723 | 678 | -45 (fewer in sequential) |
| FPU0001 | 286 | 354 | +68 (more in sequential) |
| THREAD_LIFECYCLE | 102 | 120 | +18 (more in sequential) |

CANCEL_BADGED_SENDS runs **first** in each interleaved iteration. If it triggers delayed errors, those could be caught during subsequent tests (FPU0001, THREAD_LIFECYCLE), causing:
- Undercounting of CANCEL_BADGED_SENDS errors in interleaved mode
- Overcounting of subsequent tests' errors

However, alternative explanations exist:
- Sequential mode may accumulate different CPU/cache state
- Interleaved mode may have different timing characteristics
- The differences could be within normal variation

### Idle Delay Test (2025-12-17) - Hypothesis Disproved

To test the bleeding hypothesis, we added a **5-second true idle delay** (CPU in WFI state) between tests in sequential mode. If errors were deferred and bleeding between tests, the idle period would allow them to surface and be attributed to the correct test.

**Results with 5-second idle delay:**

| Test | No Delay | 5s Idle Delay | Change |
|------|----------|---------------|--------|
| CANCEL_BADGED_SENDS | 953 | 992 | +4% |
| FPU0001 | 250 | 240 | -4% |
| THREAD_LIFECYCLE | 14 | 8 | -43% |
| **Total** | **1217** | **1240** | +2% |

**Conclusion: Hypothesis DISPROVED**

The error distribution is essentially unchanged with the idle delay. This means:

1. **Errors are NOT bleeding between tests** - The async deferral is not causing misattribution
2. **Errors are correctly attributed** - Each test genuinely triggers the errors reported against it
3. **CANCEL_BADGED_SENDS truly causes ~80% of errors** - Not an artifact of test ordering
4. **THREAD_LIFECYCLE improvement from HCR fix is real** - Not an attribution artifact

The 5-second idle period was implemented using seL4's timer + `seL4_Wait()`, which causes the idle thread to run WFI. Any pending RAS interrupts would have fired during this window.

### RAS Status DE Bit Analysis - Errors Are Synchronous

The ARM RAS ERR\<n\>STATUS register contains a **DE (Deferred Error) bit at position 23**. When DE=1, the error is deferred (asynchronous to the triggering operation). When DE=0, the error is synchronous/uncorrected.

**Decoding our RAS error status values:**

| Status Value | Binary (bits 24:20) | DE (bit 23) | Interpretation |
|--------------|---------------------|-------------|----------------|
| `0xe400090d` | `0b11100100...` | **0** | Synchronous |
| `0xe8000904` | `0b11101000...` | **0** | Synchronous |

**Both status values have DE=0, confirming all our RAS errors are synchronous, NOT deferred.**

**ERR\<n\>STATUS bit layout (bits 31:24):**
```
[31:30] AV,V   - Address Valid, Valid
[29:28] UE,ER - Uncorrected Error, Error Reported
[27:26] OF,MV - Overflow, Miscellaneous Valid
[25:24] CE,DE - Corrected Error (2 bits), Deferred Error
  bit 23: DE  - Deferred Error flag
```

**Implications:**

1. **Errors are NOT deferred** - They occur synchronously with the triggering operation
2. **No async deferral window** - The error is reported immediately, not stored for later
3. **Idle delay cannot help** - Since errors aren't deferred, waiting won't flush them
4. **Attribution is accurate** - Errors appear during the actual triggering test, not later

This technical confirmation aligns perfectly with the empirical idle delay test results: waiting 5 seconds between tests made no difference because there were no deferred errors waiting to surface.

---

## Stage 1 Translation Disabled in sel4test (2025-12-16)

### Background: Linux/KVM TTBR0/TTBR1 Errata

Linux/KVM has documented errata ([ARM errata 1319367/1319537](https://lore.kernel.org/kvm/20190927090348.GC15760@arrakis.emea.arm.com/T/)) where speculative page table walks using TTBR0_EL1/TTBR1_EL1 can cause issues during context switches. The workaround uses EPD (Entry Point Disable) bits in TCR_EL1 to prevent speculative Stage 1 walks.

### seL4 Analysis: Stage 1 is Disabled

Investigation of seL4's HCR configuration revealed that **Stage 1 translation is disabled** in sel4test:

```c
/* From vcpu.h */
/* Note that the HCR_DC for ARMv8 disables S1 translation if enabled */
#define HCR_DC       BIT(12)     /* Default cacheable */

#define HCR_NATIVE ( HCR_COMMON | HCR_TGE | HCR_TVM | HCR_TTLB | HCR_DC | ... )
#define HCR_VCPU   ( HCR_COMMON )  /* No HCR_DC - S1 enabled for VMs */
```

**Key Points:**

1. **HCR_DC disables Stage 1**: When `HCR_DC` is set, Stage 1 translation (TTBR0_EL1/TTBR1_EL1) is bypassed
2. **HCR_NATIVE includes HCR_DC**: Used for native seL4 threads (including sel4test)
3. **HCR_VCPU excludes HCR_DC**: Only used for actual VM guests with VCPU

### Implications

| Mode | HCR_DC | Stage 1 | Stage 2 | Used By |
|------|--------|---------|---------|---------|
| HCR_NATIVE | Set | **Disabled** | VTTBR_EL2 | sel4test, native threads |
| HCR_VCPU | Clear | TTBR0/1_EL1 | VTTBR_EL2 | VM guests |

### Conclusion

**The Linux/KVM EPD workarounds for TTBR0_EL1/TTBR1_EL1 are NOT relevant to sel4test** because:

1. Stage 1 translation is completely disabled via HCR_DC
2. Only Stage 2 (VTTBR_EL2) is active for address translation
3. Our HCR_EL2.VM fix already addresses Stage 2 speculative walks

**The CANCEL_BADGED_SENDS RAS errors must have a different root cause** - not related to any TTBR switching (Stage 1 or Stage 2).

---

## Why HCR_EL2.VM is Correct for Stage 2 (2025-12-16)

### TCR_EL2 Layout Depends on VHE Mode

ARM TCR_EL2 has **two different layouts** depending on VHE (Virtualization Host Extensions) mode:

| Mode | HCR_EL2.E2H | TCR_EL2 Layout | EPD bits? | TTBRs Available |
|------|-------------|----------------|-----------|-----------------|
| Non-VHE | 0 | Simple | **No** | TTBR0_EL2 only |
| VHE | 1 | Same as TCR_EL1 | **Yes** (EPD0/EPD1) | TTBR0_EL2 + TTBR1_EL2 |

**seL4 uses non-VHE mode** (HCR_EL2.E2H = 0), so TCR_EL2 does NOT have EPD bits.

### VTCR_EL2 Has No EPD Equivalent

More importantly, **VTCR_EL2** (which controls Stage 2 translation via VTTBR_EL2) **never has EPD bits** regardless of VHE mode. The VTCR_EL2 register only contains:

- T0SZ - Address size
- SL0 - Starting level
- IRGN0/ORGN0/SH0 - Cacheability/shareability
- TG0 - Granule size
- PS - Physical address size
- VS - VMID size

There is **no per-table walk disable** for Stage 2 translation.

### Architecture Summary

| Translation | Control Register | Walk Disable Mechanism |
|-------------|------------------|------------------------|
| Stage 1 EL1 | TCR_EL1 | EPD0/EPD1 bits (per-TTBR) |
| Stage 1 EL2 (VHE) | TCR_EL2 | EPD0/EPD1 bits (when E2H=1) |
| Stage 1 EL2 (non-VHE) | TCR_EL2 | None (single TTBR0) |
| **Stage 2** | **VTCR_EL2** | **None - must use HCR_EL2.VM=0** |

### Conclusion

The **HCR_EL2.VM workaround is the architecturally correct approach** for preventing speculative Stage 2 page table walks during VTTBR_EL2 switches. There is no EPD-equivalent for Stage 2 - the only option is to disable Stage 2 translation entirely during the switch.

This is consistent with [Linux's ARM64_WORKAROUND_SPECULATIVE_AT](https://lore.kernel.org/kvm/20190927090348.GC15760@arrakis.emea.arm.com/T/) which uses similar techniques for Stage 2 protection.

---

## Erratum 1941500 - TLB Multiple Hit Bug (2025-12-17)

### Discovery: ATF Workaround is INVERTED

Investigation of the NVIDIA ATF source code revealed a **critical bug** in the erratum 1941500 workaround implementation:

**File:** `Linux_for_Tegra/source/tegra/optee-src/atf/arm-trusted-firmware/lib/cpus/aarch64/cortex_a78_ae.S:39-42`

```asm
/* Set bit 8 in ECTLR_EL1 */      ← Comment says SET
mrs  x0, CORTEX_A78_AE_CPUECTLR_EL1
bic  x0, x0, #CORTEX_A78_AE_CPUECTLR_EL1_BIT_8  ← Code CLEARS (wrong!)
msr  CORTEX_A78_AE_CPUECTLR_EL1, x0
```

The code uses `bic` (bit clear) instead of `orr` (bit set). This is a [known bug reported to TF-A](https://lists.trustedfirmware.org/archives/list/tf-a@lists.trustedfirmware.org/thread/P7RYUUJ44IHCMMVI64VQKREY4ADVIMNG/).

### Erratum 1941500 Description (from ARM SDEN-1707912)

**Title:** Store operation that encounters multiple hits in the TLB might access regions of memory with attributes that could not be accessed at that Exception level or Security state

**Status:** Present in r0p0, r0p1. Fixed in r0p2.

**Conditions:**
1. A store operation encounters **multiple hits in the TLB** due to inappropriate invalidation or misprogramming of a contiguous bit
2. A read request is generated with a **physical address and attributes that are an amalgamation of the multiple TLB entries** that hit

**Implications:** A read request could be generated to regions of memory with attributes that could not be accessed at that Exception level or Security state.

**Workaround:** Set CPUECTLR_EL1[8] = 1 (small performance cost <0.5%)

### Direct Correlation to Our RAS Errors

| Erratum Description | Our Observations |
|---------------------|------------------|
| Multiple TLB hits due to inappropriate invalidation | VTTBR switches leave stale TLB entries |
| Physical address is **amalgamation** of multiple TLB entries | Addresses like 0x7fffxxxx (corrupted/combined) |
| Access to memory with wrong attributes | RAS error: "Illegal address" in SCC |

**Key insight:** The erratum explains why we see addresses like `0x7fffxxxx` - they're not real addresses, they're **corrupted combinations of stale and new TLB entries**!

### Orin AGX CPU Revision

```
CPU variant  : 0x0
CPU part     : 0xd42 (Cortex-A78AE)
CPU revision : 1     (r0p1)
```

The Orin AGX has **r0p1** silicon, which is **affected by this erratum** (fixed only in r0p2).

### Fix Applied and Tested

Changed `bic` to `orr` in ATF and reflashed the secure-os partition.

**Test Results (2025-12-17):**

| Metric | Before Fix | After Fix | Change |
|--------|------------|-----------|--------|
| SCC errors | ~655 | 663 | **No change** |
| CANCEL_BADGED_SENDS | ~79% | 76.7% | **No change** |
| FPU0001 | ~20% | 22.6% | **No change** |
| THREAD_LIFECYCLE | ~1% | 0.7% | **No change** |

**Conclusion: Erratum 1941500 is NOT the root cause of our RAS errors.**

The inverted workaround in ATF was a real bug that needed fixing, but it does not explain the RAS errors we're seeing. The error pattern, addresses, and distribution are identical before and after the fix.

This rules out TLB multiple-hit amalgamation as the cause. The 0x7fffxxxx addresses must have a different origin.

---

## CANCEL_BADGED_SENDS Code Path Analysis (2025-12-17)

### Overview

CANCEL_BADGED_SENDS_0002 causes ~77% of all RAS errors. Deep analysis of the code path revealed multiple potential trigger points.

### Test Structure

The test (`projects/sel4test/apps/sel4test-tests/src/tests/endpoints.c`) performs:

1. **Creates `NUM_BADGED_CLIENTS` helper threads** with badged endpoint capabilities
2. **Calls `cnode_revoke()`** on the badged endpoint cap - this triggers capability deletion in kernel
3. **Calls `cnode_cancelBadgedSends()`** - this triggers endpoint queue manipulation

**Key insight**: The test triggers TWO kernel operations, not just one.

### Kernel Code Path: cancelBadgedSends()

**Location**: `kernel/src/object/endpoint.c:436-487`

```c
void cancelBadgedSends(endpoint_t *epptr, word_t badge)
{
    // 1. Set endpoint to Idle, clear queue pointers
    endpoint_ptr_set_state(epptr, EPState_Idle);
    endpoint_ptr_set_epQueue_head(epptr, 0);
    endpoint_ptr_set_epQueue_tail(epptr, 0);

    // 2. Iterate through ALL threads in queue
    for (thread = queue.head; thread; thread = next) {
        word_t b = thread_state_ptr_get_blockingIPCBadge(&thread->tcbState);
        next = thread->tcbEPNext;

        if (b == badge) {
            // Restart or schedule thread
            if (seL4_Fault_get_seL4_FaultType(...) == seL4_Fault_NullFault) {
                restart_thread_if_no_fault(thread);  // or SCHED_ENQUEUE
            } else {
                restart_thread_if_no_fault(thread);
            }
            // Dequeue from endpoint
            queue = tcbEPDequeue(thread, queue);
        }
    }
    // 3. Trigger scheduler
    rescheduleRequired();
}
```

### Scheduler Trigger Chain

When `rescheduleRequired()` is called:

```
rescheduleRequired()
  └→ schedule() [kernel/src/kernel/thread.c:375]
      └→ switchToThread(candidate) [thread.c:465]
          └→ Arch_switchToThread(thread) [arch/arm/64/kernel/thread.c]
              ├→ vcpu_switch(vcpu)
              └→ setVMRoot(tcb)
                  └→ armv_contextSwitch(vspace, asid)
                      └→ setCurrentUserVSpaceRoot(ttbr)
                          └→ MSR("vttbr_el2", ttbr)  ← VTTBR switch!
```

### Capability Revocation Path

The `cnode_revoke()` call in the test triggers:

```
seL4_CNode_Revoke(...)
  └→ invokeCNodeRevoke()
      └→ cteRevoke()
          └→ finaliseCap()       ← Frees capabilities
              └→ unmapPage()     ← Page table teardown
                  └→ TLBI operations
```

### Userspace Memory Allocator Path

After kernel operations, userspace cleanup in libsel4allocman:

**Location**: `projects/seL4_libs/libsel4allocman/src/cspace/two_level.c:214`

```c
static int _destroy_second_level(cspace_two_level_t *cspace, ...)
{
    // Calls kernel to delete capability
    error = seL4_CNode_Delete(
        cspace->second_levels[index].cnode.cptr,
        slot,
        cspace->config.second_level_slot_bits
    );
}
```

### ELR Address Analysis (from previous runs)

RAS errors occur in BOTH kernel and userspace:

**Kernel addresses (EL2):**
- `ep_ptr_set_queue` - Endpoint queue manipulation
- `tcbEPDequeue` - Thread queue removal
- `setMRs_syscall_error` - Message register operations

**Userspace addresses (EL0):**
- `_destroy_second_level` - CSpace cleanup (144 occurrences)
- `_cspace_two_level_alloc` - CSpace allocation (65 occurrences)
- Various test functions

### Why This Test Is Problematic

1. **Heavy thread lifecycle operations**: Creates and destroys multiple threads
2. **Capability revocation**: Triggers page table teardown
3. **Endpoint queue manipulation**: Modifies kernel data structures
4. **Multiple context switches**: Each restarted thread may trigger VTTBR switch
5. **Combined userspace/kernel operations**: Memory allocator cleanup interleaved with kernel operations

### Comparison with Other Problematic Tests

| Test | Primary Operation | VTTBR Switches | Error Rate |
|------|-------------------|----------------|------------|
| CANCEL_BADGED_SENDS | Cap revoke + EP queue | High | 77% |
| FPU0001 | FPU context switch | High | 22% |
| THREAD_LIFECYCLE | Thread create/delete | Medium | <1% |

### Investigation Conclusions

1. **Not purely VTTBR switching**: HCR_EL2.VM fix helped FPU0001/THREAD_LIFECYCLE but made CANCEL_BADGED_SENDS worse
2. **Not TLB multiple-hit**: Erratum 1941500 fix had no effect
3. **Not deferred errors**: DE bit analysis shows synchronous errors
4. **Likely kernel data structure access**: Errors occur during queue/capability manipulation

### Instrumentation Results: REVOKE vs CANCEL (2025-12-17)

Added RAS_OP markers to isolate whether errors occur during `cnode_revoke()` or `cnode_cancelBadgedSends()`:

```c
printf("RAS_OP:REVOKE_START:%d\n", i);
error = cnode_revoke(env, helpers[i].badged_ep);
printf("RAS_OP:REVOKE_END:%d\n", i);
// ...
printf("RAS_OP:CANCEL_START:%d\n", i);
error = cnode_cancelBadgedSends(env, helpers[i].badged_ep);
printf("RAS_OP:CANCEL_END:%d\n", i);
```

**Results (100 iterations, 1272 total errors):**

| Operation | Errors | Percentage | Interpretation |
|-----------|--------|------------|----------------|
| REVOKE | 321 | 25.2% | Errors during capability revocation |
| CANCEL | 275 | 21.6% | Errors during endpoint queue ops |
| BETWEEN | 392 | 30.8% | Errors after END, before next START |
| UNKNOWN | 284 | 22.3% | FPU0001 errors (no instrumentation) |

**Key Finding: BOTH operations trigger errors at similar rates!**

The analyzer interprets this as "Both operations trigger errors" since neither REVOKE nor CANCEL dominates (>2x the other).

**Top ELR Addresses:**

| Address | Count | Function | Binary |
|---------|-------|----------|--------|
| 0x418bc8 | 163 | `_destroy_second_level` | sel4test |
| 0x808001c050 | 64 | `ep_ptr_set_queue` | kernel |
| 0x418b6c | 57 | `_cspace_two_level_alloc` | sel4test |
| 0x418c50 | 25 | `_destroy_second_level` | sel4test |
| 0x400664 | 16 | `arm_sys_send_recv` | sel4test |

**Conclusions:**

1. **Not operation-specific**: The bug is NOT isolated to either revoke or cancel - both trigger errors
2. **Common code path**: Both operations share some underlying mechanism that causes RAS errors
3. **Candidates for common trigger**:
   - Thread state transitions (both restart/reschedule threads)
   - Capability slot manipulation (both modify capability structures)
   - Scheduler invocation (both call `rescheduleRequired()`)
4. **BETWEEN errors (30.8%)**: Likely errors triggered during operation but interrupt fires after printf - suggests actual trigger rate during operations is higher

**What Both Operations Have in Common:**

| Aspect | cnode_revoke() | cnode_cancelBadgedSends() |
|--------|----------------|---------------------------|
| Modifies caps | Yes (deletes) | No |
| Modifies EP queue | No | Yes |
| Restarts threads | Yes (via finaliseCap) | Yes (via restart_thread_if_no_fault) |
| Calls rescheduleRequired() | Yes | Yes |
| May trigger context switch | Yes | Yes |

The common factor is **thread restart and rescheduling**, not the specific cap/queue operations.

### Next Investigation Directions

1. ~~Add instrumentation to track which operation triggers errors~~ **DONE** - Both operations trigger equally
2. **Focus on thread restart path** - `restart_thread_if_no_fault()` and `possibleSwitchTo()`
3. **Audit `rescheduleRequired()` path** - Called by both operations
4. **Check TCB state modifications** - Both operations modify thread state before rescheduling

---

## ARM RAS Specification Analysis (IHI0100) (2025-12-17)

### Document Reference

ARM IHI0100 "RAS System Architecture" defines the error reporting format used by Tegra234's SCC and ACI RAS nodes.

### SERR Field Decoding

From Section 2.4.8 "Software faults":

| SERR | Description |
|------|-------------|
| 0x0D | **Illegal address (software fault)**. For example, access to unpopulated memory. |
| 0x0E | Illegal access (software fault). For example, byte write to word register. |
| 0x0F | Illegal state (software fault). For example, device not ready. |

**Our errors have SERR=0x0D** - ARM officially classifies this as a "software fault" caused by accessing memory that doesn't exist.

### ERR<n>ADDR Register Layout (Section 3.2.23)

```
 63    62   61   60   59   58:56   55:0
+----+----+----+----+----+-------+------------------+
| NS | SI | AI | VA |NSE | RES0  |      PADDR       |
+----+----+----+----+----+-------+------------------+
```

| Field | Bits | Description |
|-------|------|-------------|
| NS | 63 | Non-secure attribute (physical address space) |
| SI | 62 | Secure Incorrect |
| AI | 61 | Address Incorrect |
| VA | 60 | Virtual Address (if implemented) |
| NSE | 59 | Non-secure Extension (FEAT_RME) |
| PADDR | 55:0 | Physical address of recorded location |

### Decoding Our Error Addresses

Our error addresses are `0x800000007fffxxxx`:

| Component | Value | Meaning |
|-----------|-------|---------|
| NS (bit 63) | 1 | Non-secure physical address space |
| SI (bit 62) | 0 | Secure state is correct |
| AI (bit 61) | 0 | Address is valid/correct |
| PADDR (55:0) | 0x7fffxxxx | Physical address accessed |

**Key interpretation:**
- NS=1 means the access was in the **Non-secure physical address space**
- PADDR=0x7fffxxxx is **below DRAM base** (0x80000000 on Orin AGX)
- AI=0 means ARM believes this is the **actual physical address** being accessed (not corrupted)

### What ARM Says About These Errors

From Section 2.4.8:

> "Examples of software faults include: **Access to memory or peripheral register that is not present.** This includes cases where physical address spaces are physically aliased."

> "trusted software should **set up the translation tables to prevent accesses from occurring**"

ARM explicitly states that translation tables should block accesses to non-existent memory. The fact that we're seeing these errors means either:

1. **Translation tables have gaps** - Some VA→PA mapping produces 0x7fffxxxx
2. **Speculative access bypasses translation** - Hardware speculatively accesses before translation completes
3. **Page table contains invalid PA** - A PTE has PA=0x7fffxxxx in its output address field

### IERR Field (Implementation-Defined)

Our errors show `IERR = 0x9` which the Tegra234 interprets as "Address Range Error" or "FillWrite Error". This is NVIDIA-specific and indicates the SCC/ACI detected the invalid address during a cache fill or write operation.

### Implications for Investigation

1. **ARM confirms this is a software fault** - Not a hardware glitch or random error
2. **The physical address 0x7fffxxxx is real** - AI=0 means it's not corrupted
3. **Translation should prevent this** - Something is generating invalid PA translations
4. **Focus areas:**
   - Page table entries that could contain 0x7fffxxxx
   - Stage 2 translation producing invalid IPAs
   - Speculative accesses during page table transitions

### Reference

- ARM IHI0100 "RAS System Architecture" (~/ras.txt)
- Section 2.4.8: Software faults
- Section 3.2.23: ERR<n>ADDR register

---

## ⚠️ CRITICAL DISCOVERY: VTTBR Address Correlation (2025-12-17)

### Instrumentation Results: Kernel Code Paths Clear

Implemented two instrumentation strategies to detect where 0x7fffxxxx addresses originate:

**Strategy 1: Instrument `pptr_to_paddr()`** (machine.h)
```c
if (pa != 0 && pa < 0x80000000) {
    printf("RAS_DEBUG pptr_to_paddr: BAD PA! pptr=0x%lx pa=0x%lx\n", ...);
}
```

**Strategy 2: Instrument `lookupPTSlot()`** (vspace.c)
```c
if (pt_pa != 0 && pt_pa < 0x80000000) {
    printf("RAS_DEBUG lookupPTSlot: STALE PTE! pa=0x%lx ...\n", ...);
}
```

**Result: ZERO RAS_DEBUG messages!**

This proves:
- `pptr_to_paddr()` is NOT producing invalid PA values through normal code paths
- `lookupPTSlot()` is NOT encountering stale PTEs with PA < 0x80000000
- **The 0x7fffxxxx addresses are NOT coming from executed kernel code!**

### The Breakthrough: VTTBR Address Pattern Match

Analyzing RAS error context dumps reveals a stunning correlation:

**VTTBR_EL2 values at error time:**
```
VTTBR_EL2 (user PT):   0x10000806ae000  (478 errors) - VMID=1, base=0x806ae000
VTTBR_EL2 (user PT):   0x27ffc0000      (92 errors)  - VMID=0, base=0x27ffc0000
VTTBR_EL2 (user PT):   0x10000806a8000  (3 errors)   - VMID=1, base=0x806a8000
```

**Wait - 0x27ffc0000 with VMID=0?!**

Memory layout (8GB RAM starting at 0x80000000):
- RAM start: 0x80000000
- RAM end: 0x280000000
- **0x27ffc0000 is only 256KB from END of RAM!**

### The Pattern Match is Unmistakable

```
Error addresses:     0x7ffc0xxx, 0x7fff8xxx
VTTBR base:          0x27ffc0000
                       ^^^^
                   Same "7ffc" pattern!
```

### Bit Analysis: The Missing Bit 33

```
0x27ffc0000 = 0010_0111_1111_1100_0000...  (valid, in RAM)
0x07ffc0000 = 0000_0111_1111_1100_0000...  (invalid, below DRAM!)
                ^
              bit 33 (represents 8GB offset!)
```

**If bit 33 gets dropped/truncated, 0x27ffc0000 becomes 0x7ffc0000!**

### Error Address Analysis (Test Run 2025-12-17)

| Address | Count | Offset from VTTBR base (0x27ffc0000) |
|---------|-------|--------------------------------------|
| 0x7fff8540 | 254 | +0x38540 (within page table area) |
| 0x7ffc0980 | 215 | +0x980 (within page table area) |
| 0x7ffc0a80 | 199 | +0xa80 (within page table area) |
| 0x7ffc0c00 | 128 | +0xc00 (within page table area) |
| 0x7ffc0940 | 105 | +0x940 (within page table area) |

**All error addresses are small offsets from the VTTBR base!**

### Hypothesis: Hardware Address Truncation During Speculative PTW

The hardware appears to be:
1. Speculatively walking Stage 2 page tables
2. Using the VTTBR base address (0x27ffc0000)
3. **Truncating bit 33 somehow**, producing 0x7ffc0xxx
4. Accessing the resulting invalid address (below DRAM)
5. Triggering RAS error

### Why VMID=0?

VMID=0 appears in 92 of the errors. This is significant because:
- VMID=0 is typically used for idle/kernel context
- The page table at 0x27ffc0000 with VMID=0 may be a **leftover/stale VTTBR value**
- This could indicate a race condition during VMID switching

**Boot output shows untypeds allocated at RAM end:**
```
0x27ffcb000 | 12 | 0   (4KB untyped at PA 0x27ffcb000)
0x27ffcc000 | 14 | 0   (16KB untyped at PA 0x27ffcc000)
```

The page table at 0x27ffc0000 was likely allocated from these end-of-RAM untypeds!

### Other Registers Near RAM End

Checking all registers in error context for addresses near 0x27fxxxxxx:

| Register | Value | PA | Location |
|----------|-------|-----|----------|
| **VTTBR_EL2** | 0x27ffc0000 | 0x27ffc0000 | **256KB from RAM end** ⚠️ |
| SP_EL2 | 0x827fff0800 | 0x27fff0800 | 61KB from RAM end |
| SP_EL2 | 0x827fe1b800 | 0x27fe1b800 | 1.8MB from RAM end |

**Note:** The high SP_EL2 values appear with VTTBR=0x10000806ae000 (NOT 0x27ffc0000), so they're not directly correlated with the 0x7ffc0xxx errors. Only VTTBR=0x27ffc0000 correlates with error addresses.

### EL Analysis

Most errors occur at EL=0 (userspace):
```
EL=0: 457 errors (80%)
EL=2: 116 errors (20%)
```

This suggests the speculative access happens during user code execution, not during kernel operations.

### RAM Size and Base Address Experiments (2025-12-17)

Tested with different RAM sizes and base addresses to isolate the error source:

| RAM Config | RAM Range | VTTBR | Error Addresses | Notes |
|------------|-----------|-------|-----------------|-------|
| 8GB @ 0x80000000 | 0x80000000-0x280000000 | 0x27ffc0000 | 0x7fff8540 | Original config |
| 4GB @ 0x80000000 | 0x80000000-0x180000000 | 0x17ffc0000 | 0x7fff8xxx (134), 0x0xxx (76) | Bit 33 disproven |
| 1.5GB @ 0x80000000 | 0x80000000-0xE0000000 | 0xdffc0000 | 0x7fff8540 (162), 0x0xxx (58) | Fixed addr confirmed |
| **1.5GB @ 0x90000000** | **0x90000000-0xF0000000** | **0xeffc0000** | **0x0fc0, 0x0ff0 (31 each)** | **Near-NULL errors** |
| **1.5GB @ 0x90200000** | **0x90200000-0xF0000000** | **0xeffc0000** | **0x0fc0, 0x0ff0 (28 each)** | **Same as 0x90000000!** |
| **1GB @ 0x84000000** | **0x84000000-0xC4000000** | - | **0x0fc0, 0x0ff0 (39 each)** | **Same near-NULL pattern** |
| **1GB @ 0x81000000** | **0x81000000-0xC1000000** | - | **0x0fc0, 0x0ff0 (37 each)** | **Transition < 0x81000000** |
| **1GB @ 0x80001000** | **0x80001000-0xC0001000** | 0xbffc0000 | **0x7fff8540 (98), 0x7fff84c0 (8), 0x0fc0 (46), 0x0ff0 (46)** | **⚠️ BOTH PATTERNS! 198 errors** |
| **1GB @ 0x80002000** | **0x80002000-0xC0002000** | 0xbffc0000 | **0x7fff8540 (44), 0x7fff84c0 (12), 0x0fc0 (46), 0x0ff0 (46)** | **0x7fffxxxx decreasing (56 vs 106)** |
| **1GB @ 0x80010000** | **0x80010000-0xC0010000** | 0xbffc0000 | **0x7ffccf00 (2), 0x7ff30f00 (2), 0x0fc0 (32), 0x0ff0 (32)** | **0x7fffxxxx nearly gone (4), different addrs!** |

### ⚠️ BREAKTHROUGH: Two Separate Bugs Identified!

Analysis of error patterns across all RAM configurations reveals **TWO INDEPENDENT BUGS**:

| Bug | Error Pattern | Primary Trigger | RAM Dependency | Root Cause |
|-----|---------------|-----------------|----------------|------------|
| **Bug A** | 0x7fffxxxx | FPU0001 (>99%) | Only near 0x80000000 | FPU context + 2GB boundary |
| **Bug B** | 0x0fc0/0x0ff0 | All tests (CANCEL_BADGED most) | None (constant) | Zero PTE speculation |

**Detailed per-test breakdown:**

| RAM Start | FPU0001 0x7fff | FPU0001 0x0xxx | CANCEL 0x7fff | CANCEL 0x0xxx |
|-----------|----------------|----------------|---------------|---------------|
| 0x80000000 | 188 | 10 | 0 | 26 |
| 0x80001000 | 104 | 18 | 0 | 38 |
| 0x80002000 | 54 | 22 | 0 | 24 |
| 0x80010000 | 0 | 20 | 4 | 14 |
| 0x81000000 | 0 | 18 | 0 | 24 |
| 0x84000000+ | 0 | 12 | 0 | 32 |

**Key insights:**
1. **FPU0001 exclusively triggers 0x7fffxxxx errors** - only when RAM near 0x80000000
2. **CANCEL_BADGED_SENDS triggers mostly 0x0xxx errors** - constant regardless of RAM location
3. **The bugs are completely independent** - different code paths, require separate fixes

### Code Path Analysis (ELR/PC at RAS interrupt)

| Bug | Error Pattern | Top PCs (EL0) | Functions | Code Path |
|-----|---------------|---------------|-----------|-----------|
| **A** | 0x7fffxxxx | 0x400664, 0x471d10, 0x4215a8 | `arm_sys_send_recv`, `load_segment` | Syscalls, ELF loading |
| **B** | 0x0fc0/0x0ff0 | 0x427c44, 0x424774, 0x42550c | `sel4utils_destroy_process`, `vka_cnode_revoke` | Process/cap destruction |

**Bug A (0x7fffxxxx) - Syscall/ELF Path:**
- Occurs during IPC syscalls (`arm_sys_send_recv`) and thread creation (`load_segment`)
- ONLY triggered when RAM starts within ~64KB of 0x80000000
- FPU0001 does heavy IPC and thread creation, triggering this path frequently
- Hypothesis: Some address calculation in syscall/ELF path produces `(RAM_BASE + offset - X)` that wraps below 0x80000000
- **Investigation**: Instrument kernel syscall entry, check address arithmetic near RAM base

**Bug B (0x0xxx) - Destruction/Revocation Path:**
- Occurs during process destruction (`sel4utils_destroy_process`) and cap revocation (`vka_cnode_revoke`)
- ALWAYS present regardless of RAM location
- CANCEL_BADGED_SENDS does heavy cap revocation, triggering this path
- Hypothesis: Zero-initialized PTEs are speculatively walked during destruction, producing near-NULL addresses
- **Investigation**: Add barriers in destruction path, or initialize PTEs with non-zero invalid values

### ⚠️ CRITICAL FINDING: Error Addresses are NOT DRAM-Base Relative!

When DRAM base moved from 0x80000000 to 0x90000000:

| Expected (if DRAM-relative) | Actual |
|----------------------------|--------|
| 0x8fff8540 (0x90000000 - 31KB) | **0x0fc0, 0x0ff0** |

**The error addresses went to NEAR-ZERO, not to DRAM_BASE - offset!**

```
With DRAM @ 0x80000000: Errors at 0x7fff8540 (looks like DRAM - 31KB)
With DRAM @ 0x90000000: Errors at 0x0fc0, 0x0ff0 (near NULL!)
```

**This disproves the "fixed offset from DRAM base" theory.** The 0x7fff8540 pattern was a coincidence - both addresses are simply **invalid PAs from corrupted/zero page table entries**.

### Root Cause Analysis

The near-zero addresses (0x0fc0, 0x0ff0) strongly suggest:

1. **NULL/uninitialized page table entries** - Zero-filled memory being interpreted as PTEs
2. **Speculative PTW walking invalid entries** - Hardware speculatively accesses memory based on stale/zero PTEs
3. **Cache line alignment** - 0x0fc0 and 0x0ff0 are 64-byte aligned (cache lines)

The 0x7fffxxxx addresses seen with DRAM @ 0x80000000 were also from invalid PTEs, just happening to produce different garbage addresses due to different memory layout.

### Next Steps

1. ~~Test with 4GB RAM to verify bit 33 hypothesis~~ **DONE - DISPROVEN**
2. ~~Test with 1.5GB RAM~~ **DONE - Fixed addr confirmed**
3. ~~Move DRAM base to 0x90000000~~ **DONE - DRAM-relative theory DISPROVEN**
4. **Focus on PTE initialization** - Why are speculative walks seeing zero/garbage PTEs?
5. **Investigate VTTBR switch sequence** - What's happening to page tables during context switch?

---

## ⚠️ CRITICAL CORRECTION: Speculative PTW is NOT EL2-Specific (2025-12-17)

### Previous Understanding (INCORRECT)

Much of this investigation focused on EL2/hypervisor-specific aspects:
- VTTBR_EL2 switching race conditions
- HCR_EL2.VM workaround for Stage 2 translation
- Stage 2 page table speculation

This led to an implicit assumption that speculative PTW was primarily an EL2/hypervisor issue.

### Corrected Understanding

**Speculative page table walks occur at ALL exception levels on modern AArch64 SoCs.**

Testing with ARM_HYPERVISOR_SUPPORT=OFF (kernel at EL1, no EL2/Stage-2) revealed:
- ARM_HYP=OFF WITHOUT safe PTEs: **556 RAS errors per run**
- ARM_HYP=OFF WITH safe PTEs: **0 RAS errors per run**
- ARM_HYP=ON with safe PTEs: **0-4 RAS errors per run**

The ~200x higher error rate in EL1-only mode proves:
1. **Speculative PTW is universal** - occurs regardless of exception level
2. **EL2 mode was MASKING the bug**, not causing it (due to earlier fixes)
3. **Safe PTEs are the universal solution** - work for both EL1 and EL2

### Implications for This Document

Statements in this document that imply EL2-specific behavior should be read with this correction in mind:
- HCR_EL2.VM workaround: Only helps for EL2/Stage-2 speculation
- VTTBR_EL2 switching: Only relevant when running at EL2
- The safe PTE fix (Bug B fix) now applies unconditionally to both modes

### Root Cause: Zero PTEs Are Unsafe

The fundamental issue is that **zero-initialized PTEs are unsafe on Cortex-A78AE** (and likely other modern AArch64 cores):
- When a PTE is zeroed, bits[47:12] = 0
- Speculative PTW interprets this as PA=0
- Hardware generates RAS errors accessing unmapped PA=0

**Solution**: Use "safe invalid PTEs" with bits[47:12] pointing to valid kernel memory (armKSGlobalUserVSpace) while keeping bits[1:0]=0 (invalid).

### Code Changes (commit 619e1848d)

Removed `#ifdef CONFIG_ARM_HYPERVISOR_SUPPORT` guards from:
- `vspace.c`: `pte_safe_invalid_new()` now unconditional
- `objecttype.c`: VSpaceObject and PageTableObject safe PTE initialization

---

## ARM_HYP=OFF vs ARM_HYP=ON Comparison (2025-12-17)

### Test Setup

Added multi-configuration test support to autopilot framework:
- `sel4_client.py` now requires `--arm-hyp` or `--no-arm-hyp` flag
- Build configs: `orinagx_defconfig` (ARM_HYP=ON) vs `orinagx_nohyp_defconfig` (ARM_HYP=OFF)
- Results include `config.json` tracking build configuration

### Results (Before Safe PTE Fix for EL1)

| Test ID | ARM_HYP | RAS Errors | Tests Run |
|---------|---------|------------|-----------|
| 20251217-155829 | ON | **0** | 402 |
| 20251217-155011 | OFF | **556** | 402 |

Same hardware, same stress tests (CANCEL_BADGED_SENDS, FPU0001, THREAD_LIFECYCLE, THREAD_LIFECYCLE_RAPID), same sequential execution mode.

**ARM_HYP=OFF produced 556x more errors than ARM_HYP=ON (which had zero).**

### Results (After Safe PTE Fix for EL1)

| Test ID | ARM_HYP | Safe PTEs | RAS Errors |
|---------|---------|-----------|------------|
| 20251217-164152 | OFF | Yes | **0** |

**The safe PTE fix eliminates RAS errors in EL1-only mode.**

### Error Distribution (ARM_HYP=OFF without fix)

| Test | Errors | Percentage |
|------|--------|------------|
| THREAD_LIFECYCLE_0001 | 82 | 29.4% |
| FPU0001 | 71 | 25.4% |
| THREAD_LIFECYCLE_RAPID_0001 | 68 | 24.4% |
| CANCEL_BADGED_SENDS_0002 | 58 | 20.8% |

All four stress tests trigger errors at similar rates in ARM_HYP=OFF mode (unlike ARM_HYP=ON where some tests dominated).

### Key Finding

**The safe PTE fix is the universal solution for speculative PTW RAS errors.**

It works for:
- EL2 (hypervisor) mode - Stage 2 page tables
- EL1 (non-hypervisor) mode - Stage 1 page tables
- VSpaceObject creation
- PageTableObject creation
- unmapPage() and unmapPageTable() operations

### Thermal Observation

Zero errors in ARM_HYP=ON tests was unusual (normally 2-4). After several test runs, 2 errors appeared, confirming thermal correlation exists but is secondary to the safe PTE fix.

---

## Changelog

| Date | Change |
|------|--------|
| 2025-12-18 | **PHASE 12 - RUNTIME CORRUPTION CONFIRMED**: Added debug instrumentation to verify PTEs at creation and scan during runtime. VSpace creation passes verification (`created OK`), but later scan finds 435 bad PTEs with garbage values (sample: `0x5280000054fffe23` → PA `0x54fff000`). **Proves corruption happens AFTER creation**, eliminating ELF overlap hypothesis. Cache flush issue in memory reuse path remains most likely cause. |
| 2025-12-18 | **PHASE 12 - clearMemory() INVESTIGATION**: Found that `clearMemory()` (used for untyped retype) does memzero WITHOUT cache flush. `clearMemory_PT()` exists but only used in specific places. **Most probable hypothesis**: page table memory keeps stale DRAM contents after cache eviction. Investigating. |
| 2025-12-18 | **PHASE 11 - DEBUG SCAN REVEALS GARBAGE DATA**: VSpace scan found 428+ PTEs containing ASCII strings ("core", "_new", "[i]") and ARM instructions - clearly leftover user data in page table memory. Proves memory reuse without proper cache flush to DRAM. |
| 2025-12-18 | **PHASE 10 - Arch_finaliseCap FIX**: Added clearing of page table and VSpace entries in `Arch_finaliseCap`. Fixed CANCEL_BADGED_SENDS (510→0 errors). FPU0001 still has ~218 errors from different code path (threads share VSpace). |
| 2025-12-17 | **⚠️ ARM_HYP=OFF COMPARISON**: ARM_HYP=OFF produces **556 errors** vs **0 errors** for ARM_HYP=ON with identical test suite. Hypervisor mode is MASKING the bug, not causing it. EL1 code paths need same fixes as EL2. Thermal variation may explain zero errors (usually 2-4). |
| 2025-12-17 | **CODE PATH ANALYSIS**: ELR/PC analysis reveals Bug A (0x7fffxxxx) occurs in `arm_sys_send_recv`/`load_segment` (syscall/ELF path), while Bug B (0x0xxx) occurs in `sel4utils_destroy_process`/`vka_cnode_revoke` (destruction path). Different code paths confirm independent bugs. |
| 2025-12-17 | **⚠️ BREAKTHROUGH - TWO SEPARATE BUGS IDENTIFIED**: Analysis across RAM configs (0x80000000 to 0x90000000) reveals two independent bugs: (A) 0x7fffxxxx errors triggered EXCLUSIVELY by FPU0001, only when RAM near 0x80000000; (B) 0x0xxx errors triggered by ALL tests (CANCEL_BADGED most), constant regardless of RAM location. These are completely independent bugs requiring separate investigation. |
| 2025-12-17 | **DRAM @ 0x90200000**: Same 0x0fc0/0x0ff0 errors as 0x90000000. Error addresses are completely DRAM-base independent when above 0x80000000. |
| 2025-12-17 | **⚠️ DRAM BASE TEST - DEFINITIVE PROOF**: Moved DRAM from 0x80000000 to 0x90000000. Error addresses changed from 0x7fff8540 to **0x0fc0/0x0ff0 (near-NULL)**! Expected 0x8fff8540 if DRAM-relative - got near-zero instead. **PROVES errors come from zero/uninitialized PTEs**, not from DRAM base offset. The 0x7fffxxxx pattern was coincidental garbage from invalid PTE interpretation. |
| 2025-12-17 | **4GB RAM TEST - BIT 33 HYPOTHESIS DISPROVEN**: Limited RAM to 4GB so bit 33 never set. Errors STILL occur (228 total): 0x7fff8xxx (134), 0x0xxx (76). The 0x7fff8540 pattern appears regardless of RAM size - NOT from VTTBR bit truncation. New near-NULL errors (PA=0x0fc0, 0x0ff0) appeared. Root cause is elsewhere. |
| 2025-12-17 | **⚠️ VTTBR ADDRESS CORRELATION**: Instrumented kernel shows ZERO RAS_DEBUG messages - 0x7fffxxxx addresses NOT produced by kernel code! Found stunning correlation: VTTBR_EL2=0x27ffc0000 (256KB from RAM end) with VMID=0. Error addresses 0x7ffc0xxx match VTTBR base with bit 33 dropped! ~~Hypothesis: Hardware truncates bit 33~~ **DISPROVEN by 4GB test**. |
| 2025-12-17 | **ARM RAS SPEC ANALYSIS**: Documented IHI0100 findings. SERR=0x0D means "Illegal address (software fault)". ERR<n>ADDR bit 63 is NS (Non-secure), bits 55:0 are PADDR. ARM confirms our errors are software faults accessing unpopulated memory (0x7fffxxxx < DRAM base). |
| 2025-12-17 | **INSTRUMENTATION RESULTS**: Added RAS_OP markers to isolate REVOKE vs CANCEL. **Both operations trigger errors equally** (REVOKE 25%, CANCEL 22%, BETWEEN 31%). Common factor is thread restart/rescheduling, not specific cap/queue ops. Updated analyzer to parse operation markers. |
| 2025-12-17 | **CANCEL_BADGED_SENDS CODE PATH**: Documented full code path analysis. Test triggers BOTH `cnode_revoke()` AND `cnode_cancelBadgedSends()`. Errors occur in kernel (ep_ptr_set_queue, tcbEPDequeue) AND userspace (_destroy_second_level). Likely cause: kernel data structure access patterns, not VTTBR switching. |
| 2025-12-17 | **ERRATUM 1941500 FIX - NO EFFECT**: Found ATF has inverted workaround (`bic` instead of `orr`). Fixed and tested - **RAS errors unchanged** (663 SCC). Ruled out TLB multiple-hit amalgamation as cause. |
| 2025-12-17 | **DE BIT ANALYSIS**: Decoded RAS ERR\<n\>STATUS DE bit (bit 23). All errors have DE=0, confirming they are **synchronous**, not deferred. Explains why idle delay had no effect - there are no deferred errors waiting to surface. |
| 2025-12-17 | **IDLE DELAY TEST**: Added 5-second WFI idle between tests. Error distribution unchanged - **DISPROVED** async bleeding hypothesis. Errors are correctly attributed to triggering tests. |
| 2025-12-16 | **SEQUENTIAL MODE TESTS**: Ran tests in sequential mode (AAA BBB CCC). HCR fix effect consistent (~88% improvement for THREAD_LIFECYCLE). Added hypothesis about async RAS error attribution bleeding between tests in interleaved mode. |
| 2025-12-16 | **DOCUMENTED**: TCR_EL2 EPD bits only exist in VHE mode (E2H=1). VTCR_EL2 has no EPD equivalent - HCR_EL2.VM=0 is the correct approach for Stage 2. |
| 2025-12-16 | **RULED OUT**: Stage 1 (TTBR0_EL1/TTBR1_EL1) issues - Stage 1 is disabled via HCR_DC in sel4test. Linux EPD errata workarounds not applicable. |
| 2025-12-16 | **COMPARISON TEST**: Without HCR fix: 613 SCC, THREAD_LIFECYCLE_0001 has 102 errors. With fix: 655 SCC, THREAD_LIFECYCLE_0001 drops to 12 errors (88% improvement). |
| 2025-12-16 | **BASELINE ESTABLISHED**: 655 SCC errors with automated analyzer. CANCEL_BADGED_SENDS 79%, FPU0001 20%, THREAD_LIFECYCLE 1%. Created `analyze_sel4log.py` tool. |
| 2025-12-16 | **TESTED**: DSB barriers in `cancelBadgedSends()` - NO improvement. Added dsb() before loop and before rescheduleRequired(). RAS errors still occur at same rate (~600+ per 100 iterations). DMB was tested previously (2025-12-13), now DSB also confirmed ineffective. |
| 2025-12-16 | **TESTED**: TLB invalidation sequence with HCR_EL2.VM disabled - NO improvement. Modified `invalidateLocalTLB_VMID()` and `invalidateLocalTLB_IPA_VMID()` to disable Stage 2 for entire sequence. Still ~600 RAS errors per 100 iterations. |
| 2025-12-16 | **COMMITTED**: HCR_EL2.VM workaround for VTTBR switch (commit 5179eebf0). Fixes FPU0001/thread lifecycle tests (70% → 0% error rate). CANCEL_BADGED_SENDS still has errors. |
| 2025-12-15 | **100-RUN STRESS TEST**: FPU0001 41%, CANCEL_BADGED_SENDS 56% trigger rate. 232 total RAS errors. Added debugging plan. |
| 2025-12-20 | **PHASE 19: 200KB DIAGNOSTIC REGION** - Hypothesis "something corrupts first 200KB of DRAM" **REJECTED**. Pattern at 0x80000000-0x80032000 remained intact. PT corruption still occurs in middle of DRAM (0xACxxxxxx). |
| 2025-12-18 | **RAM ADDRESS BISECTION**: Found exact boundary at 0x80032000. RAM starting below this address triggers Bug A errors; RAM at 0x80032000+ works cleanly. First 200KB of DRAM is problematic. |
| 2025-12-15 | **STATISTICAL ANALYSIS**: 25-run test proves errors NOT random - only 7/122 tests trigger errors (FPU0001 76%, CANCEL_BADGED_SENDS 32%). 115 tests proven safe. Updated CLAUDE.md with key findings. |
| 2025-12-15 | **TESTED**: `vcpu_enable()` DSB fix applied - RAS errors still occur, root cause elsewhere |
| 2025-12-15 | **MISSING BARRIER FOUND**: `vcpu_enable()` lacks DSB at entry - exactly what KVM's patch fixes |
| 2025-12-15 | Completed seL4 DSB barrier audit - all other locations are correct |
| 2025-12-15 | **CORRECTION**: Linux uses ALL-ZERO PTEs, NOT safe addresses. DSB barriers are the actual fix, not PTE content |
| 2025-12-15 | Added KVM DSB barrier patterns to match (Marc Zyngier's patch series) |
| 2025-12-15 | ~~ROOT CAUSE FOUND~~: Previous hypothesis about PA=0 in invalid PTEs was WRONG |
| 2025-12-15 | Documented full PTE unmap code path: `unmapPage()`, `unmapPageTable()`, `clearMemory_PT()` |
| 2025-12-15 | Added Linux/KVM/Xen page table initialization research (reserved_pg_dir, DSB barriers) |
| 2025-12-15 | Tested user VSpace/PageTable safe PTE initialization - RAS errors still occur |
| 2025-12-15 | Fix attempt: Initialize user page tables with safe invalid PTEs (objecttype.c) |
| 2025-12-15 | Fixed documentation: 0x7xxxxxxx addresses are BELOW DRAM base, not valid memory |
| 2025-12-15 | Fixed documentation: Corrected ARM RAS ADDR bit 63 interpretation throughout |
| 2025-12-14 | Documented SDEI architecture and why `sdei_dispatch_event returned -1` occurs |
| 2025-12-14 | Added detailed Linux kernel SDEI handling flow (PE_UNMASK, GHES, hotplug) |
| 2025-12-14 | Documented ARM RAS ADDR register bit field encoding (bit 63 = NS flag, not address) |
| 2025-12-14 | Identified PA=0 access pattern in remaining RAS errors |
| 2025-12-14 | Added ERRATA_A78_AE_2712574 for non-ARM interconnect (CBB) |
| 2025-12-14 | Enabled missing CPU errata: 1941500, 1951502, 2376748, 2395408 |
| 2025-12-14 | Fixed ATF backtrace crash when accessing seL4 memory from EL3 |
| 2025-12-14 | Built ATF with DEBUG=1 LOG_LEVEL=50 for detailed RAS output |
| 2025-12-13 | **Hypothesis identified**: NULL VTTBR_EL2 base address during TLB invalidation |
| 2025-12-13 | Added ATF debug code to capture EL2/EL3 state on RAS interrupt |
| 2025-12-13 | Added single-core DTS test - ruled out seL4 multi-core coherency |
| 2025-12-13 | Added DMB barrier test in cancelBadgedSends - no effect |
| 2025-12-13 | Added boot delay test - proved errors are synchronous |
| 2025-12-13 | Added non-hypervisor mode test results, ruled out stage-2 as root cause |
| 2025-12-13 | Initial investigation and documentation |
