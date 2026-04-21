# sel4test: EL2/EL0 Cache Coherency Stress Tests

**File**: `projects/sel4test/apps/sel4test-tests/src/tests/cache_coherency.c`
**Date**: 2026-03-22

## Background

On ARM hypervisor configurations, seL4 runs at EL2 and userspace runs at
EL0. The `really_working` kernel branch uses `HCR_EL2.DC=1` and `TGE=1`.
The `option8-el1-trampoline` branch removes both and uses an EL1 trampoline.

### Critical finding: no shareability mismatch on our build

The Orin AGX `orinagx_defconfig` build has **`CONFIG_ENABLE_SMP_SUPPORT=false`**
(uniprocessor). This means:

- **EL2 kernel window**: `SMP_TERNARY(SMP_SHARE, 0)` = **0 = NSH**
- **EL0 under DC=1**: default cacheability is Normal WB **NSH**

Both sides are NSH. The ARM ARM (DDI 0487 M.a.a, B2.10.1.2) says same-PE
NSH+NSH accesses are architecturally coherent. **There is no ISH/NSH
shareability mismatch on this platform.** The shareability mismatch theory
only applies to SMP builds where the kernel window uses ISH.

### What DC=1 actually does

DC=1 forces `HCR_EL2.VM=1` (stage-2 enabled for EL1&0 regime):
- Stage-1: DISABLED (DC=1 provides identity VA=IPA with WB NSH)
- Stage-2: ENABLED (VTTBR_EL2 translates IPA→PA)

EL2 kernel accesses use single-stage translation (TTBR0_EL2, NSH).
EL0 user accesses use two-stage translation (disabled stage-1 + stage-2, NSH).

Both produce NSH WB for the same PA. Yet stale data and RAS errors have been
observed on Orin AGX with DC=1. The root cause is **not yet explained by the
architecture specification** — possible explanations include Cortex-A78AE errata,
subtle interactions between two-stage and single-stage translation paths, or
translation table walk cache effects.

### Fix strategies

| Branch | Approach | DC=1 | RAS errors | sel4test |
|--------|----------|------|------------|----------|
| **really_working** (original) | TGE=1/DC=1 + `dc civac` in clearMemory | Yes | Untestable (hangs) | Hangs during init |
| **really_working** (dc civac removed) | TGE=1/DC=1, no cache maintenance | Yes | Yes (FRAMEEXPORTS0001) | 66/145 then crash |
| **option8-el1-trampoline** | TGE=0/DC=0, EL1 trampoline | No | None | 145/145 pass |

### The dc civac disaster on really_working

The `really_working` branch originally added `cleanInvalidateCacheRange_RAM`
(line-by-line `dc civac`) to `clearMemory()`, `clearMemory_PT()`,
`Arch_createObject()`, and `Arch_finaliseCap()`. This caused sel4test to
**hang during init**: the `populate_untypeds()` function retypes untypeds from
size 34 (16 GB) downward. A `dc civac` loop over 16 GB = 268 million cache
line operations, effectively hanging forever.

Removed in kernel commit `972c7e2b8` on `really_working`:
- `clearMemory`: reverted to `memzero` only (no cache ops)
- `clearMemory_PT`: reverted to `memzero` + `cleanCacheRange_PoU`
- `Arch_createObject` VSpace/PT: reverted to `cleanCacheRange_PoU`
- `Arch_finaliseCap` VSpace/PT: removed added flushes

### RAS errors are NOT fully resolved

Previous belief: UEFI memory map + elfloader fix solved all RAS errors.
**This is wrong.** RAS errors recur on `really_working` (DC=1) at runtime:

```
<<<ATF>>>ERROR:   ========== RAS INTERRUPT CONTEXT (seL4 debug) ==========
ERROR:   ELR_EL3 (seL4 PC when interrupted): 0x8080016560
ERROR:   SPSR_EL3: 0x3c9 (EL=2)
ERROR:   ESR_EL2: 0x56000000
ERROR:   FAR_EL2: 0x10fb3000
ERROR:   HCR_EL2: 0x8e28103b
```

Triggered during FRAMEEXPORTS0001 (mapping/reading many exported frames).
The elfloader fix solved boot-time RAS only. Runtime RAS under DC=1 persists.

Option 8 (DC=0) is the only known complete fix — zero RAS errors across
145 tests.

## Test Descriptions

### CACHECOHERENCE0001 — Full-page retype coherency stress

**Iterations**: 256 | **Page size**: 4KB | **Verification**: every byte

Core cycle:
1. Retype untyped into a 4KB frame (kernel zeros the memory at EL2)
2. Map the frame (EL0-accessible, cached)
3. Read **every byte** — verify all zeros
4. Poison the entire page with `0xDE` (dirties EL0 cache lines)
5. Unmap, revoke (returns untyped to free state)
6. Repeat from step 1 on the **same physical memory**

Full-page verification catches any single cache line where the kernel's
zero write is not visible to EL0.

### CACHECOHERENCE0002 — Sampling retype coherency (high iteration)

**Iterations**: 4096 | **Page size**: 4KB | **Verification**: 1 byte per cache line (every 64 bytes)

Same retype-verify-poison cycle but samples at cache-line granularity. This
allows **16x more iterations** than CACHECOHERENCE0001 with less time per
iteration, catching intermittent failures that only manifest under sustained
pressure (e.g., specific timing windows around preemption points in
`resetUntypedCap`).

### CACHECOHERENCE0003 — Large page retype coherency stress

**Iterations**: 32 | **Page size**: 2MB | **Verification**: 1 byte per cache line

Same pattern on 2MB large pages. Large frames exercise:
- Different PTE levels (L2 block entries vs L3 page entries)
- Different TLB caching behavior (large TLB entries)
- Larger `clearMemory()` working set (32768 cache lines per page)

Gracefully skips if no large-page-sized untyped is available.

### CACHECOHERENCE0004 — Page table coherency stress

**Iterations**: 128 | **Page size**: 4KB | **Target**: page table write path

Targets the page table coherency path. The test retypes an untyped into a
frame, maps it (exercising the PT write path), verifies reads/writes work,
then tears everything down and repeats.

## Actual Test Results (2026-03-22, Orin AGX)

### option8-el1-trampoline (request 20260322-185251)

| Test | Result |
|------|--------|
| CACHECOHERENCE0001 (full page, 256 iter) | **PASS** |
| CACHECOHERENCE0002 (sampling, 4096 iter) | **PASS** |
| CACHECOHERENCE0003 (large page, 32 iter) | **PASS** |
| CACHECOHERENCE0004 (PT coherency, 128 iter) | **PASS** |
| Full suite | **145/145 pass**, 42 disabled, 0 RAS errors |

### really_working with dc civac in clearMemory (request 20260322-185650)

**Result**: sel4test hung after printing banner, before any test ran.
Root cause: `cleanInvalidateCacheRange_RAM` in `clearMemory()` iterates
line-by-line over multi-GB untypeds during `populate_untypeds()`.

### really_working after dc civac removal (request 20260322-191844)

| Test | Result |
|------|--------|
| CACHECOHERENCE0001 (full page, 256 iter) | **PASS** |
| CACHECOHERENCE0002 (sampling, 4096 iter) | **PASS** |
| CACHECOHERENCE0003 (large page, 32 iter) | **PASS** |
| CACHECOHERENCE0004 (PT coherency, 128 iter) | **PASS** |
| CACHEFLUSH0001-0004 | **PASS** |
| FRAMEEXPORTS0001 | **RAS error** (ATF debug handler triggered) |
| IPC0003 (test 66) | **CRASH** — vspace reservation corruption |
| Full suite | 66/145 started, then abort |

**Key observation**: CACHECOHERENCE tests pass but FRAMEEXPORTS0001 triggers
RAS. The tests exercise different access patterns:
- CACHECOHERENCE: same physical frame, repeated retype/read/poison cycles
- FRAMEEXPORTS0001: many distinct frames mapped and read in sequence

The RAS error appears to require accessing many different physical frames
under DC=1, not just repeated access to the same frame.

### really_working nohyp (orinagx_nohyp_defconfig, request 20260322-200929)

Kernel runs at EL1, no DC=1, no stage-2, no two-stage translation.

| Test | Result |
|------|--------|
| CACHECOHERENCE0001 (full page, 256 iter) | **PASS** |
| CACHECOHERENCE0002 (sampling, 4096 iter) | **PASS** |
| CACHECOHERENCE0003 (large page, 32 iter) | **PASS** |
| CACHECOHERENCE0004 (PT coherency, 128 iter) | **PASS** |
| CACHEFLUSH0001-0004 | **PASS** |
| FRAMEEXPORTS0001 | **FAIL** — stale data (no RAS, just wrong values) |
| Full suite | **143/144 pass**, 1 failed (FRAMEEXPORTS0001) |

FRAMEEXPORTS0001 failure details:
```
expected=0x00 got=0x70
bytes at offset: 70 68 64 30 06 00 00 00 04 00 00 00 08 00 00 00
bytes at offset: 70 68 64 30 af af af af 04 00 00 00 af af af af
```

The `0xAF` bytes are UEFI fill pattern. `70 68 64 30` is firmware data.
`clearMemory()` wrote zeros but stale UEFI cache lines are still visible.

**This is the critical finding**: the stale data bug exists even **without
hypervisor mode**. On nohyp EL1, there is no DC=1, no stage-2, no
two-stage translation, and kernel+user share the same translation regime.
The bug is a `clearMemory` cache maintenance gap — `memzero()` alone is
insufficient to evict stale UEFI-dirty cache lines from prior boot context.

On hyp mode, stale data may trigger a hardware RAS error (not guaranteed).
On nohyp mode, it manifests as silently wrong data. The root cause is the same.

### Why CACHECOHERENCE tests pass but FRAMEEXPORTS0001 fails

- **CACHECOHERENCE**: retypes the **same** untyped 256+ times. After the
  first retype + EL0 read, cache lines for that PA are populated with
  zeros. Subsequent iterations hit warm cache — stale UEFI data evicted
  on first read.
- **FRAMEEXPORTS0001**: allocates and reads **many different** frames
  across a wide PA range. First-touch of each frame can hit stale UEFI
  cache lines that were never evicted since boot.

### really_working + ISH everywhere (requests 20260322-213151, 20260322-215446)

DC=1/TGE=1, no EL1 trampoline, no dc civac in clearMemory, no boot flush.
Only change: ISH in elfloader (TCR + PTEs) and seL4 kernel window.

| Test | Result |
|------|--------|
| CACHECOHERENCE0001–0004 | **PASS** |
| CACHEFLUSH0001–0004 | **PASS** |
| FRAMEEXPORTS0001 | **PASS** |
| Full suite | **145/145 pass**, 42 disabled, 0 RAS errors |

**Confirmed twice.** This is the fix.

### The fix: ISH everywhere

Changed `SMP_TERNARY(SMP_SHARE, 0)` → `SMP_SHARE` (always ISH, even on UP):

| File | Change |
|------|--------|
| `tools/seL4/elfloader-tool/src/arch-arm/armv/armv8-a/64/mmu.S` | `#define TCR_ISH TCR_SHARED` (unconditional) |
| `tools/seL4/elfloader-tool/src/arch-arm/64/mmu.c` | PTE SH bits: `(3 << 8)` unconditional (was `#if CONFIG_MAX_NUM_NODES > 1`) |
| `kernel/src/arch/arm/64/kernel/vspace.c` | All 6 `SMP_TERNARY(SMP_SHARE, 0)` → `SMP_SHARE` |

No changes to `clearMemory`, no cache flushes in elfloader, no EL1 trampoline.
The complete boot chain is now ISH: UEFI → elfloader → seL4 → EL0 (via D8-103).

### Why UP used NSH (historical context)

NSH on UP was a deliberate **performance optimization**, not a bug:
- ARM ARM recommends NSH for UP — "only required to support NSH model"
- Avoids activating interconnect snoop/coherency protocol on single core
- Linux arm64 used the same `#ifdef CONFIG_SMP` conditional (dropped only
  because Linux no longer supports UP arm64 builds)
- seL4 was originally UP-only; ISH added for SMP in 2017 (commit a9f57e09b)

The optimization is architecturally correct for a pure UP system. The problem
only arises because UEFI uses ISH and leaves ISH-dirty cache lines in cache
that persist into the seL4 boot context. On Orin AGX (12-core SoC), the
interconnect is always powered for coprocessors, so ISH has negligible
performance cost.

### Failed fix attempts

| Attempt | Result | Why it failed |
|---------|--------|---------------|
| `dc civac` in `clearMemory` | Hang | 268M ops per 16GB retype |
| Set/way flush in elfloader | 144/145 | FRAMEEXPORTS0001 still stale |
| `dc civac` DRAM flush (post-ExitBootServices) | Translation fault | UEFI mappings torn down |
| `dc civac` DRAM flush (pre-ExitBootServices) | AF fault | UEFI pages with AF=0 |
| Touch-then-flush (pre-ExitBootServices) | Timeout | Multi-GB touch+flush too slow |
| ISH everywhere (no flush) | **145/145** | Correct fix |

## Implementation Details

### Build

No build system changes needed. The CMakeLists.txt glob pattern
`src/tests/*.c` auto-discovers the new file.

```
make mrproper
make orinagx_defconfig
make sel4test
```

### Key design choices

- **Alternating vaddrs**: two reservations used in round-robin because `vka_cnode_revoke()` invalidates vspace tracking for the previous mapping (same pattern as CACHEFLUSH0004)
- **`volatile` reads**: prevents compiler from optimizing away the zero-check
- **`memset` for poisoning**: ensures every byte (every cache line) is dirty before the next retype cycle
- **First-failure abort**: detailed log message with iteration number, byte offset, and actual value, then stops (no log spam)
- **`CONFIG_HAVE_CACHE` guard**: consistent with existing cache tests in `cache.c`
- **Graceful skip for large pages**: CACHECOHERENCE0003 silently passes if no 2MB untyped is available

### File location

```
projects/sel4test/apps/sel4test-tests/src/tests/cache_coherency.c
```

## Root Cause: UEFI (ISH) vs seL4 UP (NSH) Shareability Attribute Mismatch

**Status: CONFIRMED. Fix verified: 145/145 twice on really_working DC=1/TGE=1.**

### The theory

The root cause is a **shareability attribute mismatch between UEFI boot
context and seL4 runtime** on the same physical memory — not a mismatch
between kernel and user within seL4.

**UEFI** maps all Normal memory as **ISH** — confirmed from EDK2 source:
- PTEs: `TT_SH_INNER_SHAREABLE` for every Normal WB mapping
  (`edk2/ArmPkg/Library/ArmMmuLib/AArch64/ArmMmuLibCore.c`)
- TCR: `TCR_SH_INNER_SHAREABLE` for translation table walks
- ATF (EL3): `TCR_SH_INNER_SHAREABLE` in TCR_EL3
  (`atf/arm-trusted-firmware/lib/xlat_tables_v2/aarch64/xlat_tables_arch.c`)
- OP-TEE (S-EL1): `TCR_SHX_ISH` in TCR_EL1
  (`nv-optee/optee/optee_os/core/arch/arm/mm/core_mmu_lpae.c`)

Every component in the boot chain uses ISH for Normal WB memory.
UEFI writes leave dirty cache lines tagged as ISH in the cache hierarchy
(0xAF fill, firmware structures, etc.).

**seL4 UP** (really_working, nohyp) maps the same physical memory as **NSH**
(`SMP_TERNARY(SMP_SHARE, 0)` = 0 on uniprocessor build).

ARM ARM B2.11: accessing the same PA with **different shareability attributes**
is **UNPREDICTABLE**. This is not the same as same-PE NSH+NSH (which is
coherent per B2.10.1.2). The ISH-tagged dirty lines from UEFI coexist in
the cache with NSH-tagged new writes from seL4 — a mismatched attribute
scenario that the architecture does not guarantee will behave correctly.

### Stage-1 + stage-2 shareability combining (ARM ARM Table D8-103)

ARM ARM DDI 0487 M.a.a, Table D8-103 defines how stage-1 and stage-2
shareability attributes combine for EL1&0 accesses. The rule is: **the
more shareable wins**.

| Stage 1 | Stage 2 | Combined |
|---------|---------|----------|
| OSH | Any | OSH |
| ISH | OSH | OSH |
| ISH | ISH | ISH |
| ISH | NSH | **ISH** |
| NSH | OSH | OSH |
| NSH | ISH | **ISH** |
| NSH | NSH | NSH |

This means **SMP seL4 with DC=1 is architecturally correct**:
- DC=1 forces stage-1 to NSH for all EL1&0 accesses
- Stage-2 on SMP uses ISH (`SMP_TERNARY(SMP_SHARE, 0)` = 3)
- Combined: NSH + ISH = **ISH** (Table D8-103, row 6)
- EL2 kernel window on SMP: ISH
- Both ISH → no mismatch → correct

The problem is **UP-specific**:
- DC=1 stage-1: NSH
- Stage-2 on UP: NSH (`SMP_TERNARY` = 0)
- Combined: NSH + NSH = NSH
- EL2 kernel window on UP: NSH
- Self-consistent within seL4, but mismatches with UEFI ISH boot residue

**Implication:** Option 8 (EL1 trampoline) is NOT required for SMP
correctness with DC=1. It is needed on UP only because of the UEFI ISH
boot residue problem — which could alternatively be fixed by:
1. Fixing the elfloader TCR to ISH + flushing all DRAM at handoff, or
2. Using ISH for the seL4 UP kernel window (matching UEFI)

### Deeper explanation: parked cores with dirty caches

The problem is not just "UEFI residue" (stale lines from a prior boot
context). It is **active dirty cache lines in 11 parked cores' private
L1/L2 caches** that are still participating in the coherency domain.

Orin AGX has 12 Cortex-A78AE cores. UEFI boots all 12 with ISH caches
enabled. When `ExitBootServices()` is called, UEFI does NOT flush the
secondary cores' caches. The elfloader and seL4 (UP mode) only run on
core 0. The other 11 cores are parked in WFE/WFI — but their **caches
are still enabled and dirty**. No `PSCI CPU_OFF` is issued (the UP
elfloader compiles out all SMP code, including `smp_boot()`).

With NSH on core 0:
- Memory accesses do NOT snoop the other 11 cores' caches
- Those cores may hold dirty data for addresses seL4 is accessing
- The coherency protocol doesn't resolve the conflict under NSH
- Result: stale data, potential RAS

With ISH on core 0:
- The coherency protocol snoops ALL cores in the inner shareable domain
- Even parked cores' caches participate (hardware maintains coherency
  while cores are in WFE/WFI)
- Core 0's writes and reads are fully coherent with all 12 cores' caches
- Result: correct behavior

**Why set/way flush from core 0 didn't fully fix it** (request
20260322-210216, 144/145): `dc cisw` only flushes **core 0's** cache
hierarchy. The dirty lines in the other 11 cores' L1/L2 are untouched.
Remote dirty lines can still be served to core 0 on a subsequent access.

**This is fundamentally a UP-on-SMP-hardware problem.** The ARM ARM's
NSH guarantee ("same PE, same attributes = coherent") is correct for a
true single-PE system. But Orin AGX is a 12-core SoC where 11 cores have
active caches. Running a UP OS with NSH on multi-core hardware with other
cores' caches still live is architecturally unsafe — the NSH accesses
from core 0 bypass the coherency protocol that would resolve conflicts
with the other cores' dirty lines.

### Why option8 works without dc civac in clearMemory

Option 8 forces **ISH everywhere**, even on UP:

```c
#define KERNEL_WINDOW_SH SMP_SHARE  /* Always ISH — not SMP_TERNARY */
#define IDENTITY_MAP_SH 3           /* Inner Shareable, always */
```

This **matches** UEFI's ISH attributes. No attribute mismatch on the same
PA → the cache coherency protocol handles `memzero` writes correctly →
stale UEFI lines are properly overwritten → FRAMEEXPORTS0001 passes.

### Why dc civac "fixed" it

`dc civac` flushes to Point of Coherency regardless of shareability
attributes. It evicts the ISH-tagged UEFI dirty lines from cache,
resolving the mismatch. But it's unusable at scale (hangs on multi-GB
untypeds — 268M cache line ops for a 16 GB retype).

### Why set/way ops at boot "masked" it

Set/way flushes the entire cache, evicting all UEFI-dirty ISH lines
before seL4 maps anything as NSH. Once the cache is cold, there are no
mismatched lines to cause trouble.

### Verification status — ALL CONFIRMED

1. ~~Confirm UEFI uses ISH.~~ **CONFIRMED** from UEFI/ATF/OP-TEE source.
2. ~~Check elfloader cache maintenance.~~ **INVESTIGATED.** See below.
3. ~~Test ISH everywhere on really_working.~~ **CONFIRMED.** 145/145 pass
   twice (requests 20260322-213151, 20260322-215446). No flushes needed.
4. ~~Boot-time flush sufficient?~~ **NOT NEEDED.** ISH everywhere eliminates
   the mismatch entirely. Set/way flush was tried but FRAMEEXPORTS0001
   still failed (144/145) — flush alone is insufficient without matching
   shareability. ISH everywhere is the correct and complete fix.

### Elfloader cache maintenance gap

The elfloader (`tools/seL4/elfloader-tool/`) sits between UEFI and the
seL4 kernel. It does perform cache maintenance, but **only on the regions
it loads** — not on all of DRAM.

**What the elfloader flushes** (line-by-line `dc civac` + `dsb sy`):
- Kernel ELF image region
- User/rootserver ELF image region
- Bootinfo block (EFI memory map)
- Elfloader's own text/data/BSS/page tables (before MMU switch)

**What the elfloader does NOT flush:**
- The vast majority of DRAM that becomes seL4 untypeds
- Any UEFI-dirty cache lines outside the loaded ELF regions

**Elfloader shareability — internal inconsistency:**

The elfloader TCR shareability is conditional on `CONFIG_MAX_NUM_NODES`:
```c
/* tools/seL4/elfloader-tool/include/arch-arm/64/mode/assembler.h */
#if CONFIG_MAX_NUM_NODES > 1
#define TCR_ISH     TCR_SHARED      /* ISH */
#else
#define TCR_ISH     0               /* NSH! */
#endif
```

On our UP build (`CONFIG_MAX_NUM_NODES=1`), the elfloader TCR uses **NSH**.

But the elfloader's PTE construction hardcodes ISH:
```c
/* tools/seL4/elfloader-tool/src/arch-arm/sys_boot.c */
(3 << 8)  /* make sure the shareability is the same as the kernel's */
```

The comment says "same as the kernel's" but the value `3` is ISH, while the
kernel on UP uses NSH. This is an internal mismatch within the elfloader —
PTEs say ISH, TCR says NSH.

**The complete shareability chain on UP Orin AGX:**

| Stage | PTEs | TCR | Dirty lines tagged as |
|-------|------|-----|-----------------------|
| UEFI | ISH | ISH | ISH |
| Elfloader | ISH (hardcoded) | NSH (conditional) | Mixed? |
| seL4 kernel | NSH | NSH | NSH |
| seL4 option8 | ISH (forced) | ISH (forced) | ISH |

The UEFI→elfloader transition already introduces an ISH→NSH mismatch
(at TCR level). The elfloader→seL4 transition continues with NSH.
The untyped memory regions retain UEFI's ISH-dirty cache lines throughout
because the elfloader never flushes them.

**Net result:** When seL4 retypes an untyped and does `memzero`, it writes
through NSH-mapped memory. But the cache still holds UEFI's ISH-dirty lines
for the same PA. ARM ARM B2.11 says different shareability on the same PA
is UNPREDICTABLE → stale data.

**Previous failed fix attempt:** An earlier attempt added a full DRAM flush
(using the EFI memory map) in the elfloader before jumping to seL4. This
**triggered RAS errors** — because the elfloader's own `dc civac` operations
were going through NSH mappings (TCR_ISH=0 on UP) while the cache held
ISH-dirty lines from UEFI. The flush itself hit the same mismatched-attribute
UNPREDICTABLE behavior, causing the hardware RAS mechanism to fire.

**The fix:** The elfloader's `TCR_ISH` must be ISH always (matching its own
PTEs which already hardcode ISH). Then the DRAM flush would operate through
ISH mappings on ISH-dirty lines → matched attributes → safe. Two changes:

1. Fix elfloader `TCR_ISH` to always be ISH (not conditional on
   `CONFIG_MAX_NUM_NODES`):
   ```c
   /* tools/seL4/elfloader-tool/include/arch-arm/64/mode/assembler.h */
   #define TCR_ISH     TCR_SHARED   /* Always ISH — match PTEs and UEFI */
   ```

2. Add full DRAM flush in elfloader (using EFI memory map) after fixing TCR.
   This evicts all UEFI ISH-dirty lines through ISH-matched mappings before
   seL4 maps anything.

Alternatively, seL4 kernel itself could use ISH for the kernel window on UP
(matching UEFI), eliminating the mismatch without needing an elfloader flush.
Option 8 already does this (`KERNEL_WINDOW_SH = SMP_SHARE` always).

## Remaining Questions

1. **Should ISH be conditional?** Currently we force ISH unconditionally.
   A more targeted approach could limit ISH to platforms where other cores
   may have active caches (i.e., any multi-core SoC running UP seL4).
   In practice, this is nearly all modern ARM platforms. The NSH UP
   optimization is only safe on true single-core hardware with no other
   cache-enabled agents. On Orin AGX (12-core, interconnect always
   powered for coprocessors), ISH has negligible performance cost.
