# Orin AGX arm64 Page-Table Walker PoC vs PoU Study (2026-03-14)

## Scope

This document records an investigation of the claim made in seL4 kernel commit
`eba5a3b6e6b804a77c6cfc8d2a6535707c953c9a`:

- subject: `arm: Fix page table cache coherency - use dc civac (PoC) not dc cvau (PoU)`
- key claim: the ARM64 hardware page-table walker reads from Point of
  Coherency (PoC), not Point of Unification (PoU), therefore page-table
  updates must be cleaned to PoC to be visible.

The goal here is to determine whether that statement is architecturally true,
and whether modern coherent SoCs such as NVIDIA Orin AGX / Cortex-A78AE still
require PoC cache maintenance for normal runtime arm64 page-table updates.

## Short Answer

The strong claim in commit `eba5a3b6e6b804a77c6cfc8d2a6535707c953c9a` is not
supported by the best available evidence.

The evidence supports a narrower conclusion instead:

- Normal arm64 runtime publication of page-table entries is primarily a
  barrier/shareability/coherency-domain problem.
- On coherent arm64 systems, page-table updates are generally made visible to
  the hardware walker with the architected barrier sequences used by Linux
  (`dsb ishst`, plus `isb`/TLBI where applicable), without mandatory PoC
  clean/invalidate of the page-table memory itself.
- Cleaning page tables to PoC is a conservative workaround that may mask
  platform-specific or implementation-specific visibility problems, but it is
  not the general architectural rule for arm64 runtime page-table updates.
- PoC is still explicitly required in some different situations, especially
  boot / MMU-off handoff and other cases involving system caches or non-CPU
  coherent agents.

## Local seL4 Evidence

The commit under investigation says the walker reads from PoC. However, the
 current tree already contains newer comments that contradict that statement.

### seL4 commit text

Command used:

```bash
git -C /home/hlyytine/tii-sel4/kernel show --stat --format=fuller eba5a3b6e6b804a77c6cfc8d2a6535707c953c9a
```

The commit message states:

- "The ARM64 MMU hardware page table walker reads from Point of Coherency
  (PoC), not Point of Unification (PoU)."
- therefore `dc civac` is required instead of `dc cvau`.

### Current seL4 comments no longer say that

`include/arch/arm/arch/machine.h` now says:

- we use `dc civac` / PoC for page-table maintenance
- while the MMU hardware walker architecturally operates at PoU
- PoC is chosen because it is more conservative on complex SoCs

Reference:

- `/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/machine.h:52`

This is the most important local contradiction. The tree no longer consistently
supports the original commit message's architectural claim.

## Upstream Linux arm64 Evidence

The strongest evidence came from upstream-style Linux arm64 code in the active
kernel tree under:

- `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra`

Linux arm64 is important here because its page-table publication paths track
architectural requirements closely.

### 1. Fresh page-table pages are published with `dsb ishst`, not PoC clean

In `arch/arm64/mm/mmu.c`, Linux allocates a page-table page, relies on it being
zeroed, and then performs only:

```c
/* Ensure the zeroed page is visible to the page table walker */
dsb(ishst);
```

Reference:

- `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/mm/mmu.c:405`

If runtime arm64 page-table visibility architecturally required cleaning the
page-table memory all the way to PoC, this code would be fundamentally wrong.
It is not doing `dc cvac`, `dc civac`, or `dc cvau` here.

Historical source:

- commit `32d6397805d00573ce1fa55f408ce2bca15b0ad3`
- subject: `arm64: mm: ensure that the zero page is visible to the page table walker`

### 2. Valid kernel PTE publication uses barriers, not PoC clean

In `arch/arm64/include/asm/pgtable.h`, Linux publishes valid kernel PTEs with:

```c
WRITE_ONCE(*ptep, pte);
if (pte_valid_not_user(pte)) {
    dsb(ishst);
    isb();
}
```

Reference:

- `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/include/asm/pgtable.h:255`

Again, this does not use PoC cache maintenance for ordinary runtime page-table
publication.

Historical source:

- commit `7f0b1bf04511348995d6fce38c87c98a3b5cb781`
- subject: `arm64: Fix barriers used for page table modifications`

That commit message explicitly frames the issue as a required barrier sequence
between page-table modifications and subsequent accesses, not as mandatory PoC
cache clean for page tables.

### 3. Linux arm64 still has real PoU primitives for CPU cache coherence

In `arch/arm64/mm/cache.S`, Linux defines `dcache_clean_pou()` and on systems
with `IDC` that operation can collapse to just `dsb ishst`.

Reference:

- `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/mm/cache.S:124`

This is relevant because it shows Linux still distinguishes PoU and PoC in the
normal arm64 cache-maintenance model and does not treat all coherency problems
as requiring PoC.

## Boot-Time Evidence: Where PoC Is Explicitly Required

Linux arm64 boot requirements explicitly require the loaded kernel image to be
cleaned to PoC:

- "The address range corresponding to the loaded kernel image must be cleaned
  to the PoC."

Reference:

- `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/Documentation/arm64/booting.rst:180`

This is an important distinction:

- runtime page-table publication under a coherent enabled MMU regime is one
  problem
- boot / MMU-off visibility in the presence of system caches is another

The existence of explicit PoC requirements for boot is evidence that Linux
documents PoC when it is actually required. It does not document runtime
arm64 page-table publication that way.

## Interpretation

The best-supported interpretation is:

- arm64 runtime page-table visibility is normally guaranteed through coherent
  memory attributes, correct shareability, and the required barrier sequence
  before the walker can observe the new entries
- the statement "the walker reads from PoC, not PoU" is too strong and is not
  consistent with upstream arm64 publication paths
- `dc civac` may still fix a real bug in a specific kernel or SoC integration,
  but that does not prove the generic architectural claim

Likely alternate explanations when `dc civac` appears to fix the problem:

- missing or misplaced `dsb ishst`
- missing `isb` or TLBI ordering on the consuming path
- page-table memory mapped with wrong cacheability/shareability attributes
- stale data surviving due to a non-standard system-cache behavior
- a boot-time / MMU-transition issue being mistaken for a normal runtime
  page-table-publication issue

## Orin AGX / Cortex-A78AE Implications

### What is modern about this platform

Orin AGX is based on Tegra234, which uses Cortex-A78AE CPU cores. This is a
modern coherent arm64 SoC class, not an old non-coherent ARM integration.

Public evidence used:

- Jetson AGX Orin uses Cortex-A78AE cores
- Cortex-A78AE is an Arm automotive core intended for coherent DynamIQ / DSU-AE
  class systems

### Practical conclusion for Orin AGX

For normal runtime page-table updates on Orin AGX / Cortex-A78AE class
platforms, explicit PoC clean of page tables should generally not be required
as the architectural baseline.

What still remains true:

- PoC may still be a conservative workaround
- PoC may still be needed for boot and MMU-off transitions
- PoC may still be needed for interactions with system caches or other coherent
  masters outside the simple CPU-private PoU story

But that is different from claiming that the arm64 page-table walker
architecturally reads from PoC.

## Final Conclusion

The investigation result is:

1. The commit message in `eba5a3b6e6b804a77c6cfc8d2a6535707c953c9a` overstates
   the architectural claim.
2. The current seL4 tree already contains a more defensible framing:
   architecturally the walker is treated as operating at PoU, while PoC is
   used as a conservative choice.
3. Upstream Linux arm64 behavior strongly supports the barrier-based model for
   runtime page-table visibility.
4. For modern coherent SoCs such as Orin AGX / Cortex-A78AE, PoC should not be
   assumed necessary for ordinary runtime page-table publication, though it may
   still be useful or required in boot / platform-specific edge cases.

## References

### Local repository references

- seL4 commit under study:
  - `/home/hlyytine/tii-sel4/kernel` commit `eba5a3b6e6b804a77c6cfc8d2a6535707c953c9a`
- seL4 current comment contradicting the original claim:
  - `/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/machine.h:52`
- Linux arm64 zero-page / page-table-walker visibility:
  - `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/mm/mmu.c:405`
- Linux arm64 PTE publication barriers:
  - `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/include/asm/pgtable.h:255`
- Linux arm64 PoU cleaning primitive:
  - `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/mm/cache.S:124`
- Linux arm64 boot requirement for PoC:
  - `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/Documentation/arm64/booting.rst:180`

### External references

- Linux arm64 booting documentation:
  - https://docs.kernel.org/arch/arm64/booting.html
- Linux cache/TLB documentation:
  - https://docs.kernel.org/core-api/cachetlb.html
- arm64 patch thread: `arm64: mm: ensure that the zero page is visible to the page table walker`
  - https://www.spinics.net/lists/arm-kernel/msg454826.html
- arm64 patch thread: `arm64: Fix barriers used for page table modifications`
  - https://www.spinics.net/lists/arm-kernel/msg1011093.html
- Arm community discussion on MMU/cache visibility symptoms:
  - https://community.arm.com/support-forums/f/architectures-and-processors-forum/48858/armv8-mmu-problem
- Cortex-A78AE product page:
  - https://www.arm.com/products/silicon-ip-cpu/cortex-a/cortex-a78ae
- public kernel discussion referencing Tegra234 / Cortex-A78AE:
  - https://www.spinics.net/lists/kernel/msg6015079.html

## Confidence / Limits

Confidence is high on the negative conclusion:

- the investigation did not find support for the strong generic statement that
  the arm64 page-table walker reads from PoC rather than PoU
- the investigation did find strong upstream Linux evidence for the opposite
  practical model

Limit:

- the exact ARM ARM wording was not quoted directly here because the
  architecture manual text is not conveniently available in a publicly
  searchable form in this workspace
- therefore the conclusion relies on public Linux arm64 implementation,
  Arm-authored patch discussions, and current seL4 comments rather than a
  direct ARM ARM quotation
