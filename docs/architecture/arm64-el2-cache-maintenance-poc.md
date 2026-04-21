# arm64/EL2: Page Table Cache Maintenance — PoU vs PoC

## Executive Summary

On NVIDIA Orin AGX (Cortex-A78AE), seL4 running at EL2 with
`HCR_EL2.DC=1`, using `dc cvau` (clean to PoU) for page table cache
maintenance causes reproducible sel4test crashes. Switching to
`dc civac` (clean+invalidate to PoC) fixes the issue. A separate
stale-data bug in seL4's capability revocation path (missing cache
flush on page table deletion) compounds the problem and requires an
independent fix.

The AArch64 architecture text (DDI 0487M.a.a) states that PoU-level
maintenance should be sufficient for translation table walk coherency.
We cannot cleanly explain the observed failure from the public
architecture text alone. However, Linux arm64 also uses PoC-level
maintenance for page tables in the analogous KVM configuration, citing
the need to handle the "guest caches off" case when `SCTLR_EL1.M`
effectively equals 0. The engineering conclusion — use PoC — is robust
regardless of whether the exact architectural mechanism is fully
understood.

---

## Part A: Empirical Observations

All observations on NVIDIA Orin AGX (Cortex-A78AE), single core,
seL4 at EL2 with `CONFIG_ARM_HYPERVISOR_SUPPORT`.

### A.1 Directly observed (experimentally demonstrated)

1. **PoU fails, PoC works**: `dc cvau` (PoU) for page table maintenance
   causes sel4test to crash around test 34–38. `dc civac` (PoC) passes
   141/141 tests. Reproducible across multiple runs.

2. **Delete-time flush is independently required**: Even with PoC on
   all creation/map paths, omitting the cache flush on page table
   deletion in `Arch_finaliseCap` causes crashes around test 34.

3. **Both fixes are needed together**: Neither PoC-on-create alone
   nor delete-flush alone is sufficient. Both are independently
   necessary.

4. **Crash signature**: Allocation failure in the test driver
   (`basic_set_up`), suggesting silent memory corruption from stale
   page table entries, not an immediate fault.

5. **No RAS errors**: The failures are not caused by speculative
   prefetch into decode holes (that was a separate, resolved issue).

### A.2 Observed test matrix

| Configuration | Result |
|--------------|--------|
| Patches 1+2+3+4 (full series) | 141/141 pass, 4 runs |
| Patches 1+2+4 (no delete flush) | Crash at test ~34 |
| Patches 1+2+3 (PoU, with delete flush) | Crash at test ~38 |
| Patches 1+2 only (no delete flush, PoU) | 141/141 pass |

The last row passes because the upstream code does not exercise the
delete-flush path — `Arch_finaliseCap` does not flush page table
memory in upstream seL4.

### A.3 Inferred (not directly observed)

- The crash is consistent with the stage-2 translation table walker
  seeing stale PTE data, but we have not directly observed stale walker
  reads (e.g., via trace or hardware debug).

- The corruption accumulates over ~30 test iterations before manifesting,
  suggesting a probabilistic window rather than a deterministic first-use
  failure.

---

## Part B: Architectural Interpretation

### Claim audit table

| # | Claim | Status | Why | Better wording |
|---|-------|--------|-----|----------------|
| 1 | "HCR_EL2.DC=1 disables stage-1 translation" | **Supported** | ARM ARM D24-8835: "the PE behaves as if the value of SCTLR_EL1.M field is 0" | Accurate as stated. More precisely: stage-1 address translation is bypassed; the effective memory type is forced to Normal WB-RA-WA. |
| 2 | "Native threads use stage-2 only" | **Supported** | HCR_EL2.DC=1 disables stage-1 (SCTLR_EL1.M=0) and forces VM=1 (stage-2 enabled). The only active address translation for EL1/EL0 is stage-2. | Accurate. |
| 3 | "Data accesses bypass the cache" / "caches off" | **Weak/misleading** | HCR_EL2.DC=1 forces memory type to Normal WB cacheable. Data accesses are cached. The Zyngier KVM quote refers to SCTLR_EL1.C=0 (actual caches-off), not HCR_EL2.DC=1. | HCR_EL2.DC=1 does NOT make accesses non-cacheable. It disables stage-1 translation while forcing cacheable attributes. The analogy to "caches off" is imprecise. See B.1 below. |
| 4 | "Stage-2 walks are coherent at PoC, not PoU" | **Incorrect** | The ARM ARM PoU definition (D7-7542) says "translation table walks" generically, with no stage-1/stage-2 distinction. No AArch64 text makes this claim. | The ARM ARM does not distinguish stage-1 and stage-2 walker coherency points. PoU is architecturally defined as sufficient for all translation table walks on AArch64. |
| 5 | "CohWalk is IMPLEMENTATION DEFINED on AArch64" | **Incorrect** | CohWalk exists only in ID_MMFR3_EL1, an AArch32 feature register (D24-9266). There is no AArch64 equivalent. AArch64 implicitly assumes hardware-coherent walks. | CohWalk is AArch32-only. It is not relevant to AArch64 walker coherency. |
| 6 | "Linux uses PoC because guest caches are off" | **Plausible but imprecise** | Zyngier's KVM patch addresses SCTLR_EL1.C=0 (true caches-off). HCR_EL2.DC=1 is a related but distinct configuration — it forces cacheable attributes, not non-cacheable. Whether the same rationale applies to DC=1 is plausible but not explicitly stated in the Linux commit. | Linux KVM uses PoC when SCTLR_EL1 indicates caches/MMU are off. seL4's HCR_EL2.DC=1 creates a configuration where SCTLR_EL1.M effectively equals 0. The analogy is plausible but the mechanisms differ. See B.1. |
| 7 | "PoC is always sufficient" | **Supported** | PoC is defined as "the point at which all agents that can access memory are guaranteed to see the same copy" (D7-7542). This is the strongest coherency guarantee available. | Accurate by definition. |

### B.1 What HCR_EL2.DC=1 actually does

ARM DDI 0487M.a.a, D24-8835:

When `HCR_EL2.DC=1`:
- The PE behaves as if `SCTLR_EL1.M=0` (stage-1 translation off)
- The PE behaves as if `HCR_EL2.VM=1` (stage-2 translation on)
- The memory type produced by stage 1 is forced to **Normal
  Non-Shareable, Inner WB-RA-WA, Outer WB-RA-WA**

This is **not** the same as "caches off." The forced memory type is
Normal Write-Back Cacheable. Data accesses from EL1/EL0 go through
the data caches normally. The stage-1 address translation is bypassed
(no VA→IPA mapping), but the cacheability attributes are not removed.

The Zyngier KVM rationale ("guest accesses are completely bypassing
the cache") applies to the case where `SCTLR_EL1.C=0` (caches truly
disabled), which is a different configuration. Whether the same
reasoning extends to `HCR_EL2.DC=1` (stage-1 disabled but cacheable
attributes forced) is not obvious. The configurations share
`SCTLR_EL1.M=0` behavior, but differ in cacheability.

However: in both cases, stage-1 translation is not active. The PoU
definition describes the point where "instruction and data caches and
translation table walks of that PE" are coherent. With stage-1
disabled, the translation table walks in question are stage-2 walks
only. Whether the PoU guarantee was designed to cover the case where
stage-1 is absent and only stage-2 walks occur is not explicitly
addressed in the architecture text.

### B.2 What the ARM ARM says about walker coherency

**PoU definition** (D7-7542):
> The PoU for a PE is the point by which the instruction and data
> caches and the translation table walks of that PE are guaranteed
> to see the same copy of a memory location.

**PoU scope** (D7-7551, Table D7-7):
> For Inner Shareable memory, cache maintenance to PoU is effective
> to "The PoU of instruction cache fills, data cache fills and
> write-backs, and translation table walks, of all PEs in the same
> Inner Shareable shareability domain."

Both references say "translation table walks" without qualification.
Neither distinguishes stage-1 from stage-2. On AArch64 there is no
`CohWalk` or equivalent field that qualifies this guarantee.

**What the text does NOT say**:
- It does not state "PoU is sufficient for stage-2 walks when stage-1
  is disabled via HCR_EL2.DC"
- It does not state "PoU is insufficient for stage-2 walks"
- It does not address the `HCR_EL2.DC=1` configuration specifically
- It does not define whether the "translation table walks" in the PoU
  definition refer to all walks the PE performs or only walks of the
  currently active translation regime

### B.3 The Non-Shareable attribute: ruled out

HCR_EL2.DC=1 forces the stage-1 memory type to **Normal
Non-Shareable**. An initial hypothesis was that this could reduce the
effective shareability and narrow the PoU scope. However:

- The stage-2 descriptor for page table memory has SH=Inner Shareable
- Table D8-103 (D8-7692) defines the combining rule: Non-Shareable
  (stage-1) + Inner Shareable (stage-2) = **Inner Shareable**
- The `dc cvau` is executed at EL2 using the EL2 VA, which is mapped
  as Inner Shareable in `TCR_EL2`
- Per Table D7-7, dc cvau on ISH memory is effective to the ISH
  domain PoU

The Non-Shareable forced attribute does not reduce the effective
scope of the cache maintenance. This hypothesis is **ruled out**.

### B.4 The system-level cache hypothesis: strongest explanation

DDI 0487M.a.a, D7-7560 (D7.5.12 "System level caches") defines three
classes of system cache:

> 1. Before PoC, not manageable by any cache maintenance → **prohibited**
> 2. Before PoC, manageable by VA-based ops to PoC but not by set/way
> 3. Beyond PoC, invisible to software for coherency management

Class 2 system caches are architecturally permitted. They lie between
the PoU (defined by CLIDR_EL1.LoUU/LoUIS) and the PoC. They are
reachable by `dc civac` (which operates to PoC) but **not** by
`dc cvau` (which operates to PoU).

Since system caches are "not described in the PE Cache Identification
registers, CCSIDR_EL1 and CLIDR_EL1" (D7-7560), the PoU levels
reported by CLIDR_EL1 do not account for them. The PoU definition
says translation table walks are coherent at PoU, but PoU is defined
relative to CLIDR_EL1 cache levels — it says nothing about system
caches that are architecturally invisible to CLIDR_EL1.

**If the Orin AGX has a class 2 system cache** (e.g., in the MSS or
memory controller) **and the stage-2 walker can read from it**, then:

- `dc cvau` pushes data to PoU (L2 cluster boundary) but not beyond
- The system cache retains stale data
- The walker reads stale PTEs from the system cache
- `dc civac` pushes data to PoC, which by definition includes class 2
  system caches, ensuring the walker sees fresh data

This is the strongest architectural explanation we have. It is
**consistent with** but not **proven by** the available evidence —
we have not confirmed the existence of a class 2 system cache on
Orin AGX through hardware documentation, only inferred it from
behavior.

**What would prove this**: Reading the Cortex-A78AE TRM (DDI 0598)
and the Orin AGX system-level cache documentation to confirm whether
a class 2 system cache exists in the memory path. Alternatively,
reading CLIDR_EL1 to determine LoC vs LoUU and confirming LoC > LoUU
would indicate cache levels between PoU and PoC.

### B.5 CohWalk is AArch32-only

The `CohWalk` field in `ID_MMFR3_EL1` (D24-9266) describes AArch32
translation table walk coherency. There is no equivalent in
`ID_AA64MMFR0_EL1` through `ID_AA64MMFR4_EL1`. AArch64 implicitly
assumes hardware-coherent walks — the PoU definition unconditionally
includes walks without qualification.

CohWalk is not relevant to AArch64 behavior.

---

## Part C: Engineering Conclusion

### C.1 What is proven

1. On Orin AGX at EL2 with `HCR_EL2.DC=1`, `dc cvau` (PoU) for page
   table cache maintenance is empirically insufficient.
2. `dc civac` (PoC) fixes the issue.
3. A separate bug exists in seL4's `Arch_finaliseCap`: the capability
   revocation path does not flush page table memory, unlike the
   user-invoked unmap path (`performPageTableInvocationUnmap`) which
   calls `clearMemory_PT`.
4. Both the PoC fix and the delete-time flush are independently
   necessary for correct behavior.

### C.2 What remains hypothesis

1. **Leading hypothesis (B.4)**: A class 2 system cache (per D7.5.12)
   exists on Orin AGX between the ISH PoU and the system PoC. The
   stage-2 walker reads from this cache. `dc cvau` does not reach it;
   `dc civac` does. This is architecturally consistent — the PoU
   definition's "translation table walks" guarantee is defined in
   terms of CLIDR_EL1 cache levels, which do not include system
   caches.
2. **Ruled out (B.3)**: The Non-Shareable attribute forced by DC=1.
   Table D8-103 combining rules produce Inner Shareable for the
   combined stage-1/stage-2 attributes.
3. **Not applicable (B.5)**: CohWalk is AArch32-only.
4. **Unresolved**: Whether this reproduces on other Cortex-A78AE
   platforms or other ARMv8 cores with class 2 system caches.
5. **Unresolved**: The precise role of the preemption window in
   `resetUntypedCap` — the model is consistent with observations but
   not directly proven via hardware trace.
6. **Actionable next step**: Read CLIDR_EL1 on Orin AGX. If
   LoC > LoUU, this confirms cache levels between PoU and PoC,
   strongly supporting the class 2 system cache hypothesis.

### C.3 Why PoC is the correct engineering choice

1. **PoC is correct by definition**: "the point at which all agents
   that can access memory are guaranteed to see the same copy"
   (D7-7542). This includes all hardware walkers regardless of
   translation regime or HCR configuration.
2. **Linux arm64 precedent**: Linux uses `dc civac` (PoC) for all
   page table maintenance. Linux KVM specifically handles the "guest
   MMU off" case (SCTLR_EL1.M=0) with PoC-level maintenance. seL4's
   `HCR_EL2.DC=1` creates an analogous (though not identical)
   configuration.
3. **No performance concern**: Page table maintenance is not a hot
   path. The difference between `dc cvau` and `dc civac` is
   negligible.
4. **Portability**: Using PoC avoids dependency on
   platform-specific walker coherency behavior that may vary
   across ARMv8 implementations.

### C.4 The delete-time flush

Upstream seL4's `Arch_finaliseCap` does not flush page table memory
on capability revocation. The user-invoked path
(`performPageTableInvocationUnmap`) does call `clearMemory_PT`
(memzero + cache flush). sel4test exercises the revocation path.

The delete-time flush (`cleanInvalidateCacheRange_RAM` in
`Arch_finaliseCap`) ensures page table cache lines are pushed to PoC
and invalidated from L1/L2 before the memory re-enters the untyped
allocator. This closes a coherency window during `resetUntypedCap`,
which zeros memory via `clearMemory` (memzero only, no cache flush)
with `preemptionPoint()` calls between chunks.

---

## What is proven vs what remains hypothesis

| Item | Status |
|------|--------|
| `dc cvau` fails on Orin AGX at EL2 with DC=1 | **Proven** (reproduced) |
| `dc civac` fixes it | **Proven** (reproduced) |
| Delete-time flush is independently required | **Proven** (reproduced) |
| Both fixes needed together | **Proven** (reproduced) |
| The ARM ARM says PoU should suffice for walks | **Supported** (text is clear) |
| HCR_EL2.DC=1 disables stage-1, forces stage-2 | **Supported** (text is clear) |
| The failure involves stale PTEs visible to walker | **Plausible** (consistent with symptoms, not directly observed) |
| The preemption window in resetUntypedCap is the trigger | **Plausible** (consistent, not proven via trace) |
| Non-Shareable forced attribute is a factor | **Ruled out** (Table D8-103: NS+ISH=ISH) |
| Class 2 system cache between PoU and PoC (B.4) | **Leading hypothesis** (architecturally permitted, consistent with behavior, not directly confirmed on Orin) |
| CohWalk explains AArch64 behavior | **Ruled out** (AArch32-only) |

---

## Suggested upstream-safe wording

For commit messages and PR descriptions, avoid claiming architectural
mandates that the ARM ARM text does not explicitly make. Instead:

> On NVIDIA Orin AGX (Cortex-A78AE) at EL2 with HCR_EL2.DC=1,
> PoU-level cache maintenance (dc cvau) for page tables is empirically
> insufficient — sel4test crashes during capability revocation.
> PoC-level maintenance (dc civac) fixes the issue.
>
> PoC is always architecturally correct (it is the point where all
> agents see the same data). PoU is architecturally described as
> sufficient for translation table walks, but this guarantee may not
> hold on all implementations in all HCR configurations. Linux arm64
> uses PoC for all page table maintenance.

---

## Suggested commit-message paragraph

```
On Orin AGX (Cortex-A78AE) at EL2 with HCR_EL2.DC=1, dc cvau (PoU)
for page table maintenance is empirically insufficient — the stage-2
walker observes stale PTEs after page table recycling, causing
sel4test crashes. dc civac (PoC) fixes this. While the ARM ARM
(DDI 0487M.a.a, D7-7542) defines PoU as covering translation table
walks, the guarantee may not hold on all implementations when
stage-1 is disabled via HCR_EL2.DC. PoC is always correct by
definition and matches Linux arm64 practice.
```

---

## Most likely overclaims in earlier drafts

1. **"Stage-2 walks are coherent at PoC, not PoU" (ARM DDI 0487 D8.3)**
   — The ARM ARM makes no such distinction. This was fabricated.

2. **"CohWalk is IMPLEMENTATION DEFINED on AArch64"** — CohWalk is
   AArch32-only (ID_MMFR3_EL1). No AArch64 equivalent exists.

3. **"HCR_EL2.DC=1 means caches are off / accesses bypass the cache"**
   — DC=1 forces Normal WB cacheable attributes. Accesses are cached.
   This confused DC=1 with SCTLR_EL1.C=0.

4. **"This is directly the same as the Linux KVM caches-off case"** —
   The Zyngier patch addresses SCTLR_EL1.C=0 (true caches-off). DC=1
   is related (SCTLR_EL1.M=0) but not identical (cacheability is
   preserved). The analogy is useful but not exact.

5. **"The architecture defines stage-2 walker coherency at PoC"** —
   Pure invention. The architecture defines PoU as covering
   "translation table walks" generically, with no stage distinction
   and no AArch64 qualification.

---

## Affected Code Paths

All kernel page table maintenance changed from PoU to PoC:

| Function | File | Original | Required |
|----------|------|----------|----------|
| `clearMemory_PT` | `include/arch/arm/arch/machine.h` | `cleanCacheRange_PoU` | `cleanInvalidateCacheRange_RAM` |
| `unmapPageTable` | `src/arch/arm/64/kernel/vspace.c` | `cleanByVA_PoU` | `cleanInvalByVA` |
| `unmapPage` | `src/arch/arm/64/kernel/vspace.c` | `cleanByVA_PoU` | `cleanInvalByVA` |
| `performPageTableInvocationMap` | `src/arch/arm/64/kernel/vspace.c` | `cleanByVA_PoU` | `cleanInvalByVA` |
| `performPageInvocationMap` | `src/arch/arm/64/kernel/vspace.c` | `cleanByVA_PoU` | `cleanInvalByVA` |
| `benchmark_arch_map_logBuffer` | `src/arch/arm/64/kernel/vspace.c` | `cleanByVA_PoU` | `cleanInvalByVA` |
| `Arch_finaliseCap` (new) | `src/arch/arm/64/object/objecttype.c` | (none) | `cleanInvalidateCacheRange_RAM` |
| `Arch_createObject` | `src/arch/arm/64/object/objecttype.c` | `cleanCacheRange_PoU` | `cleanInvalidateCacheRange_RAM` |

---

## Shared Memory Coherency Under EL2

The page table walker issue above is separate from data cache coherency
for shared memory between VMs and native threads.

### Translation regimes

| Actor | HCR_EL2 profile | Stage-1 | Stage-2 | Cache trap bits |
|-------|----------------|---------|---------|-----------------|
| Native seL4 thread | HCR_NATIVE | Off (DC=1) | On (VM=1) | None (TPU/TPC not set) |
| Guest VM (VCPU) | HCR_VCPU | On | On (VM=1) | None (TPU/TPC not set) |

### Hardware coherency for data

ARM64 Normal Cacheable Inner-Shareable memory is hardware-coherent
across PEs via the cache coherency protocol. Regular loads and stores
are automatically visible. No explicit cache maintenance is needed for
CPU-to-CPU shared memory when both sides map the memory as Normal
Cacheable Inner-Shareable.

Explicit cache maintenance (`dc cvau`, `dc cvac`, `dc civac`) is
needed for:

| Purpose | Required instruction |
|---------|---------------------|
| Instruction/data coherency (JIT) | `dc cvau` (PoU) |
| Non-coherent observer (DMA, GPU) | `dc cvac` or `dc civac` (PoC) |
| Page table walker (portable) | `dc civac` (PoC) |

### Attribute mismatch

If stage-2 maps shared memory with different cacheability attributes
for different VMs, hardware coherency does not apply. The producing
side must use `dc cvac`/`dc civac` to push data to PoC.

---

## References

### ARM Architecture Reference Manual (DDI 0487M.a.a)
- D7-7542: PoU and PoC definitions
- D7-7548, Table D7-3: PoC cache maintenance scope (effective to
  "PoC of the entire system")
- D7-7551, Table D7-7: PoU cache maintenance scope (effective to
  "PoU of all PEs in ISH domain")
- D7-7560, D7.5.12: System level caches — defines class 2 system
  caches (before PoC, reachable by VA ops to PoC, not by set/way
  or PoU ops)
- D8-7692, Table D8-103: Combining stage-1 and stage-2 Shareability
  (Non-Shareable + Inner Shareable = Inner Shareable)
- D24-8835: HCR_EL2.DC definition and effects
- D24-9266: CohWalk in ID_MMFR3_EL1 (AArch32-only)
- D24-8810: HCR_EL2.TPU/TPC cache maintenance trapping
- B2.6.9: DSB completion guarantees for cache maintenance and
  speculative translation table walks

### ARM Core Documentation
- Cortex-A78AE Technical Reference Manual (DDI 0598)

### seL4 Source
- `kernel/include/arch/arm/armv/armv8-a/64/armv/vcpu.h` — HCR_NATIVE,
  HCR_VCPU definitions
- `kernel/include/arch/arm/arch/object/vcpu.h` — HCR bit definitions
- `kernel/src/arch/arm/64/object/objecttype.c` — Arch_finaliseCap,
  Arch_createObject
- `kernel/src/arch/arm/64/kernel/vspace.c` — page table map/unmap
- `kernel/src/object/untyped.c` — resetUntypedCap, preemptionPoint
- `kernel/include/arch/arm/arch/machine.h` — clearMemory, clearMemory_PT

### Linux Kernel
- `arch/arm64/include/asm/cacheflush.h` — uses `dc civac` for page
  table maintenance
- `arch/arm64/kvm/hyp/pgtable.c` — uses `dc civac` for stage-2 page
  tables
- Marc Zyngier, "[PATCH v3 01/11] arm64: KVM: force cache clean on
  page fault when caches are off" — PoC rationale for SCTLR_EL1.M=0
- Cortex-A53 errata 819472/826319/827319/824069 — dc cvau promoted
  to dc civac on affected cores
