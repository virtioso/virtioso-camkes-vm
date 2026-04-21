# Orin AGX arm64 Speculative PTW Commit Study (2026-03-14)

## Scope

This document studies the following seL4 kernel commits in
`/home/hlyytine/tii-sel4/kernel`:

- `13647827189ee78f03c45e9bf4e252989ab3b296`
- `efdce541a481b59285cf70b2766e38b2efc77ad3`
- `1bf44e5573d0fb1b9e450a063a0e569e8616c02f`
- `0d5cee14837ca4fe75fd5361df9838952321465d`

Question under study:

- Do these fixes correspond to issues already recognized by peer projects
  such as Linux or Xen?
- Or are they better understood as local bandaids over a deeper problem?

## Executive Summary

The four commits do not all fall into the same category.

### Main result

- `0d5cee148` is in a class of problems that Linux and Xen both clearly
  acknowledge: speculative translation activity while switching TTBR/VTTBR or
  while temporarily operating in a mixed translation regime.
- `136478271` is not corroborated by Linux or Xen as normal arm64 practice.
  I found no peer implementation that encodes a non-zero "safe PA" inside
  otherwise invalid arm64 PTEs to protect speculative walkers.
- `efdce541a` and `1bf44e557` are mixed:
  - the "initialize page-table memory before exposure" part is absolutely
    consistent with Linux practice
  - the "initialize with safe invalid PTEs that carry a non-zero PA payload"
    part is not corroborated by Linux or Xen and looks workaround-like

### High-level classification

1. `0d5cee148`
   - mostly a real fix in a known class
   - exact seL4 implementation is platform-shaped, but not obviously a hack

2. `136478271`
   - best understood as a bandaid / platform-specific mitigation

3. `efdce541a`
   - partly real fix, partly bandaid

4. `1bf44e557`
   - partly real fix, partly bandaid

## The Commits

### 1. `136478271`: override invalid PTE constructor with a "safe PA"

Current code:

- `/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/64/mode/machine.h:23`

The commit overrides `pte_pte_invalid_new()` to produce an invalid descriptor
whose address bits point at `armKSGlobalUserVSpace` instead of zero:

```c
#define pte_pte_invalid_new() \
    ((pte_t){ .words[0] = addrFromPPtr(armKSGlobalUserVSpace) & 0xfffffffff000ull })
```

Claim:

- speculative page-table walks on Cortex-A78AE may read invalid PTEs
- zero payload causes a speculative walk toward PA 0
- therefore invalid PTEs should carry a non-zero safe base address

### 2. `efdce541a`: initialize global empty vspace with those safe invalid PTEs

Current code:

- `/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c:299`

The global user vspace is filled at boot with `pte_pte_invalid_new()`.

### 3. `1bf44e557`: initialize newly created VSpace/PageTable objects with safe invalid PTEs

Current code:

- `/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c:452`
- `/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c:476`

Newly retyped VSpace/PageTable objects are filled with `pte_pte_invalid_new()`
before publication.

### 4. `0d5cee148`: use a valid page-table base during VMID switch for TLBI

Current code:

- `/home/hlyytine/tii-sel4/kernel/include/arch/arm/armv/armv8-a/64/armv/tlb.h:18`

When temporarily switching VMID for TLBI, seL4 now uses:

```c
setCurrentUserVSpaceRoot(ttbr_new(vmid, addrFromPPtr(armKSGlobalUserVSpace)));
```

instead of `base = 0`.

Claim:

- some implementations, notably Tegra234/Orin, may perform speculative table
  walks during TLBI
- VTTBR/TTBR base 0 can therefore trigger RAS errors

## What Linux Clearly Acknowledges

Linux arm64 recognizes several closely related hazards.

### A. Freshly zeroed page-table memory must be visible to the walker

Linux arm64 does this explicitly:

- `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/mm/mmu.c:405`

```c
/* Ensure the zeroed page is visible to the page table walker */
dsb(ishst);
```

This is directly recognized in public arm64 mailing-list discussion:

- Will Deacon patch:
  - https://www.spinics.net/lists/arm-kernel/msg467697.html
- Mark Rutland review:
  - https://www.spinics.net/lists/arm-kernel/msg467709.html
- follow-up:
  - https://www.spinics.net/lists/arm-kernel/msg468124.html

This is important for `efdce541a` and `1bf44e557`: initializing page-table
memory before exposing it to the walker is normal and necessary.

### B. Reserved / empty root tables are used to avoid speculative fetches

Linux arm64 explicitly installs a reserved TTBR0 root:

- `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/include/asm/mmu_context.h:38`

```c
/* Set TTBR0 to reserved_pg_dir. No translations will be possible via TTBR0. */
```

Linux also documents why:

- `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/include/asm/mmu_context.h:85`

```c
/* ... To avoid issues resulting from speculative TLB fetches, we must
 * temporarily install the reserved page tables ... */
```

Public thread:

- https://www.spinics.net/lists/arm-kernel/msg472087.html
- https://www.spinics.net/lists/arm-kernel/msg467292.html

This is strong support for the general idea behind `0d5cee148`: when changing
translation context, using a valid empty root instead of something pathological
is normal peer practice.

### C. KVM has explicit workarounds for speculative translation activity with guest VMIDs

Linux KVM nVHE contains this comment:

- `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/kvm/hyp/nvhe/tlb.c:24`

```c
/* For CPUs that are affected by ARM 1319367, we need to
 * avoid a host Stage-1 walk while we have the guest's
 * VMID set in the VTTBR in order to invalidate TLBs. */
```

Linux handles this by blocking further S1 walks with `TCR_EPD0/1`, not by
writing fake addresses into invalid descriptors.

Public trace of the same workaround class:

- https://www.spinics.net/lists/arm-kernel/msg802959.html

This is again strong support for `0d5cee148` being a real issue class, even if
seL4's exact implementation differs from Linux.

### D. Linux does not treat invalid PTEs as needing special "safe PA" payloads

Linux arm64 publication barriers are aimed at valid kernel mappings:

- `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/include/asm/pgtable.h:259`

Recent Linux discussion is explicit that barriers are for written values to be
observed by the walker for valid mappings, and are avoided for invalid or
userspace mappings:

- https://www.spinics.net/lists/arm-kernel/msg1079634.html
- https://www.spinics.net/lists/arm-kernel/msg1079768.html

This matters because it is the opposite of the seL4 "safe invalid descriptor"
idea. Linux does not appear to encode a non-zero safe base into invalid
descriptors as a standard mitigation.

## What Xen Clearly Acknowledges

Xen also acknowledges speculative translation hazards during translation-regime
changes.

### A. Xen carries the same speculative-AT workaround class

Patchew mirrors Xen changes such as:

- `xen/arm: Add workaround for Cortex-A55 erratum #1530923`
  - https://patchew.org/Xen/61a105672650e7470710183f37351b821b818d1e.1606215998.git.bertrand.marquis%40arm.com/

The patch description states:

- speculative AT can allocate TLB entries during guest context switch
- this can happen with inconsistent guest page-table state
- Xen uses `ARM64_WORKAROUND_AT_SPECULATE`

This is a direct peer acknowledgment that speculative translation activity
around guest context switching is a real problem class.

### B. Xen also treats TTBR switching as delicate

Patchew mirrors a Xen series:

- `xen/arm: Don't switch TTBR while the MMU is on`
  - https://patchew.org/Xen/20230416143211.72227-1-julien%40xen.org/

That discussion frames TTBR switching as something requiring a strict
break-before-make style sequence and temporary identity mapping.

This reinforces the same conclusion as Linux:

- switching translation bases is a real hazard class
- peer projects solve it with empty/reserved roots, identity maps,
  break-before-make rules, or speculative-walk suppression

### C. What I did not find in Xen

I did not find evidence that Xen standardizes the specific seL4 technique of:

- encoding a non-zero safe PA inside otherwise invalid PTEs

So Xen corroborates the broad hazard class, but not the specific mechanism
used by `136478271`.

## Commit-by-Commit Assessment

### `0d5cee148`: real fix in a recognized class

Assessment:

- This is not merely a random bandaid.
- Linux and Xen both acknowledge that speculative translation activity during
  TTBR/VTTBR / VMID switching is real and dangerous.
- Linux uses reserved page tables and, for erratum cases, suppresses further
  walks while the guest VMID is active.
- Xen carries analogous speculative-AT and TTBR-switching precautions.

What is specific to seL4:

- seL4's exact tactic is "use `armKSGlobalUserVSpace` instead of base 0"
- Linux often uses reserved empty roots or disables further walks
- Xen uses workaround logic around context switch / TTBR handling

Conclusion:

- `0d5cee148` is best understood as a legitimate fix for a recognized class of
  problems, with a seL4-specific implementation choice

### `136478271`: likely workaround over a deeper issue

Assessment:

- I found no Linux or Xen evidence that arm64 invalid descriptors should carry
  a non-zero safe PA payload.
- Peer projects appear to assume that zeroed invalid entries are fine, provided
  page-table memory is correctly initialized, published, and walked under the
  right barriers/attributes.

So if `136478271` fixes observed RAS events on Orin/A78AE, the more likely
interpretation is:

- a platform-specific speculative walker / RAS reporting quirk is being masked
- or some deeper seL4 issue existed around uninitialized/stale PT memory,
  wrong root installation, or visibility sequencing

Conclusion:

- `136478271` looks like a bandaid/platform-specific mitigation, not a
  generally corroborated arm64 fix pattern

### `efdce541a`: half real fix, half workaround

Assessment:

- Real fix part:
  - keeping `armKSGlobalUserVSpace` initialized before use is normal and
    consistent with Linux practice
  - empty reserved roots should not contain random stale memory
- Workaround part:
  - filling it with "safe invalid PTEs" rather than ordinary zero-invalid
    descriptors is not corroborated by Linux/Xen

Conclusion:

- as an initialization fix, yes
- as validation of the "safe PA in invalid PTE" idea, no

### `1bf44e557`: half real fix, half workaround

Assessment:

- Real fix part:
  - newly allocated / retyped page-table objects must not expose stale memory
    to the walker
  - Linux also relies on newly allocated page-table pages being zeroed and then
    made visible to the walker
- Workaround part:
  - using specially crafted invalid descriptors with a non-zero embedded PA is
    not mirrored by Linux or Xen

This strongly suggests a deeper underlying issue existed:

- stale page-table memory from untyped reuse
- publication/visibility sequencing
- or a platform-specific speculative walk / RAS quirk

Conclusion:

- the initialization aspect fixes a real bug
- the "safe invalid PTE payload" aspect still looks workaround-like

## Bottom Line

The best-supported interpretation is:

1. `0d5cee148` fixes a problem class that Linux and Xen both already recognize.
   It is not just a random bandaid.
2. `136478271` is not corroborated as normal peer practice and looks like a
   local mitigation for Orin/A78AE behavior or for another deeper issue.
3. `efdce541a` and `1bf44e557` combine:
   - a real fix: do not expose stale/uninitialized page-table memory
   - a workaround: encode non-zero "safe" addresses in invalid descriptors

If these commits were being upstreamed, I would expect:

- `0d5cee148` to be arguable on the merits, especially if tied to a specific
  CPU erratum or reproducible platform behavior
- `136478271` / the "safe invalid PTE" portions of `efdce541a` and
  `1bf44e557` to face skepticism unless backed by a documented CPU erratum or
  stronger architectural evidence

## References

### Local repository references

- seL4 safe invalid PTE constructor:
  - `/home/hlyytine/tii-sel4/kernel/include/arch/arm/arch/64/mode/machine.h:23`
- seL4 boot init of global empty vspace:
  - `/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/kernel/vspace.c:299`
- seL4 object init for VSpace/PageTable:
  - `/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c:452`
  - `/home/hlyytine/tii-sel4/kernel/src/arch/arm/64/object/objecttype.c:476`
- seL4 VMID switch using valid empty root:
  - `/home/hlyytine/tii-sel4/kernel/include/arch/arm/armv/armv8-a/64/armv/tlb.h:18`
- Linux reserved TTBR0:
  - `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/include/asm/mmu_context.h:38`
- Linux speculative TLB fetch comment:
  - `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/include/asm/mmu_context.h:85`
- Linux KVM speculative VMID/VTTBR workaround:
  - `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/kvm/hyp/nvhe/tlb.c:24`
- Linux zeroed PT visible to walker:
  - `/home/hlyytine/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra/arch/arm64/mm/mmu.c:405`

### External references

- Linux arm64 zero page visible to PTW:
  - https://www.spinics.net/lists/arm-kernel/msg467697.html
  - https://www.spinics.net/lists/arm-kernel/msg467709.html
  - https://www.spinics.net/lists/arm-kernel/msg468124.html
- Linux reserved page tables for speculative TLB fetches:
  - https://www.spinics.net/lists/arm-kernel/msg472087.html
  - https://www.spinics.net/lists/arm-kernel/msg467292.html
- Linux barrier discussion focused on valid mappings:
  - https://www.spinics.net/lists/arm-kernel/msg1079634.html
  - https://www.spinics.net/lists/arm-kernel/msg1079768.html
- Linux KVM speculative AT / VTTBR workaround trace:
  - https://www.spinics.net/lists/arm-kernel/msg802959.html
- Xen speculative-AT workaround:
  - https://patchew.org/Xen/61a105672650e7470710183f37351b821b818d1e.1606215998.git.bertrand.marquis%40arm.com/
- Xen TTBR switching discussion:
  - https://patchew.org/Xen/20230416143211.72227-1-julien%40xen.org/

## Confidence / Limits

Confidence is high on these points:

- `0d5cee148` matches a real, peer-recognized problem class
- the "safe invalid PTE payload" technique is not corroborated by the Linux
  and Xen evidence gathered here

Limitations:

- I did not find an official public ARM ARM quotation specifically endorsing
  or forbidding non-zero payloads in invalid descriptors for speculative PTW
- I also did not find official Xen source-browser pages for the exact same
  mechanism as seL4; Xen evidence here comes from Patchew mirrored patch
  discussions
