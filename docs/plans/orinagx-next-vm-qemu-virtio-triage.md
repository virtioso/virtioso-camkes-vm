# Orin AGX `vm_qemu_virtio` `orinagx-next` Triage

## Purpose

This note classifies the local `orinagx-next` branch material that may be
needed to run `vm_qemu_virtio` on NVIDIA Orin AGX while rewriting the current
manifest-level `virtioso-next` stack.

The key rule is selective replay. Do not merge or replay `orinagx-next`
wholesale: it mixes real Orin enablement with cacheability experiments,
RAS-error hunting, and debug-only tracing paths that were created while the
underlying shareability bug was still misunderstood.

## Branches Checked

| Repository | Current observation | Triage role |
| --- | --- | --- |
| `kernel` | replayed selected Orin DTS/platform completeness on top of `virtioso-next` | keep platform/DTS, avoid SDEI/RAS/cache experiments |
| `tools/seL4` | left untouched; current `virtioso-next` already carries the newer elfloader Orin/MMU/memmap line | treat as current shareability baseline unless runtime evidence says otherwise |
| `projects/virtioso-camkes-vm` | current branch is `virtioso-next`; Orin app config is preserved and `clean_cache` is disabled for the first trial | avoid replaying older template/code over newer contract-based DT generation |
| `projects/vm` | replayed Orin VM glue on top of `virtioso-next` | source for VMM-side Orin platform, cross-VM, TCU, and DTB-dump support |
| `projects/seL4_libs` | `virtioso-next` already contains the NULL-vspace reservation fix as `5d68a4c` | no new replay needed |
| `virtioso-build` | `orinagx_defconfig` now points at the Yocto sysroot dt-bindings path | keep defconfig/layer facts, avoid SDEI/RAS auto-selection |

## Current Replay Status

Status as of 2026-04-27:

- `projects/vm/virtioso-next` now carries the selected Orin VMM glue, including
  the DTB dump hook requested from `ec6d103`, the 8-bit-safe cross-VM IRQ, the
  optional control dataport, TCU/PMC guest-FDT support, and ARM VM PCI module
  enablement.
- `kernel/virtioso-next` now carries the selected Orin DTS completeness commits
  without the old SDEI/RAS debug handler line and without the manual
  cache-maintenance experiments.
- `projects/virtioso-camkes-vm/virtioso-next` keeps the existing Orin
  `vm_qemu_virtio` composition, consolidates TCU DT generation into the
  Virtioso-owned FDT hook, and sets VM0/VM1 `clean_cache` to `false` for the
  first validation trial.
- `virtioso-build/virtioso-next` uses the existing Yocto sysroot
  `dt-bindings` directory. A fresh `make linux-image` was not required for this
  build because the required headers already existed under
  `vm-images/build/tmp/sysroots-components/`.
- `projects/seL4_libs/virtioso-next` already contains the NULL-vspace
  `sel4utils_elf_reserve()` fix, so the historical `c8a90b0` cherry-pick was
  empty.
- Clean build validation now passes:

```sh
make mrproper
make orinagx_defconfig
make vm_qemu_virtio
```

The produced image is
`orinagx_vm_qemu_virtio/images/capdl-loader-image-arm-orinagx`.

The next proof step is Autopilot runtime validation with chain
`vm-qemu-virtio`. If that fails with cache/coherency-like symptoms, retry with
`clean_cache=1` as the controlled comparison.

## Keep Or Replay

### Kernel Platform and DTS Completeness

Keep the Orin platform support needed for normal boot and guest device
passthrough:

- Cortex-A78 / Tegra234 platform identity.
- Tegra Combined UART (`TCU`) serial support.
- GIC path and platform constants.
- Orin DTS nodes needed by the VMM and Linux guests: HSP top/AON, BPMP, PMC,
  SBSA UART, SDMMC4, MGBE/P2U, MC/EMC, SMMU nodes, SRAM/BPMP shared memory, and
  dt-bindings includes.

Evidence commits from `kernel/orinagx-next`:

- `7f648bc02 arm: Add NVIDIA Orin AGX (Tegra234) platform support`
- `be93e2765 arm: Add Cortex-A78 CPU support`
- `b537280aa orinagx: Add BPMP/HSP device nodes for VM guest support`
- `409280639 orinagx: Add SBSA UART node for VM console`
- `a64d771e5 orinagx: Fix GIC path and update TCU/memory layout in DTS`
- `ac679b90a orinagx: Add SDMMC4 (internal eMMC) node to DTS`
- `ab72e2b37 orinagx: Add MGBE0 Ethernet and P2U PHY to DTS`
- `fbf3c5de6 orinagx: Add dt-bindings support for DTS compilation`
- `c840b5a3c orinagx: Add complete NVIDIA ethernet overlay properties`
- `bd43dcf25 orinagx: Add SMMU and MC nodes to DTS for debug passthrough`
- `b35e01de1 orinagx: Complete DT nodes for SMMU, MC, MGBE, and MMC`
- `ad745fe6b orinagx: Fix TCU node`
- `7c1dcf4b6 orinagx: Pass real PMC node through`

Do not take the later RAS-oriented platform config as part of this group.

### Elfloader Boot and Shareability Boundary

Keep the elfloader work that makes the boot mapping and memory-map handoff sane
on Orin. This is separate from the rejected cacheability attempts in the VMM or
guest transport.

Evidence commits from `tools/seL4/orinagx-next`:

- `78710e7 elfloader: add NVIDIA Orin AGX platform`
- `77d279d elfloader: add Tegra Combined UART driver`
- `66bc2a6 arm64: quiesce bootloader MMU state`
- `6b9c8fb arm64: build elfloader identity map for cached execution`
- `9fa1310 elfloader/aarch64: use VA-based cache maintenance`
- `93bb598 elfloader: capture UEFI memory map and pass to kernel`
- `e9d692e elfloader: align EFI memory regions to 2 MiB boundaries`
- `bdf5bc0 arm_enable_hyp_mmu: accept page table base as parameter`
- `ee7435b aarch64: Limit address mappings in elfloader`

This group is the likely home of the real Orin boot/cache/shareability fix. It
should be replayed or re-derived before revisiting any old guest-side
cacheability workaround.

Handle this group carefully. Elfloader changes affect the physical/virtual
execution environment before the kernel starts, so replaying them mechanically
can easily hide whether the real dependency is platform bringup, UEFI memory-map
handoff, MMU quiescing, or the shareability fix. Prefer small commits with a
clear boot-stage rationale and validate each one against the Orin clean build
path.

### VMM-Side Orin VM Glue

Keep the Orin-specific VMM support that provides real `vm_qemu_virtio`
composition on the target board:

- Orin `vmlinux.h` platform header in `projects/vm`.
- 8-bit-safe cross-VM IRQ reserve, currently IRQ `236`.
- Optional control dataport plumbing for the cross-VM connector.
- TCU console support through the platform FDT customization path when still
  needed by the active CAmkES composition.
- PMC/MAC address guest DTB customization if Ethernet passthrough remains part
  of the Orin validation target.
- Reuse existing `/chosen` nodes rather than rebuilding them blindly.
- Optional guest DTB dump support for debugging generated FDT contents during
  Orin bringup.

Evidence commits from `projects/vm/orinagx-next`:

- `97e8adf orinagx: reserve 8-bit-safe crossvm irq for vm_qemu_virtio`
- `600bf95 vm: plumb optional control dataport for crossvm`
- `8bcd8ea orinagx: Add legacy CAmkES virtio support`
- `93b11b4 orinagx: Add TCU console support via fdt_plat_customize hook`
- `cfce264 orinagx: Add PMC node and MAC address to guest DTB`
- `b37080e orinagx: Move passthrough devices from vmlinux.h to devices.camkes`
- `cdaeee1 Use existing chosen node if available`
- `ec6d103 VM_Arm: Add DTB dump feature for debugging`

Current `projects/virtioso-camkes-vm/virtioso-next` already has a substantial
Orin `apps/Arm/vm_qemu_virtio/orinagx/devices.camkes`, including VM0/VM1 memory
layout, TCU console, MGBE/SDMMC/HSP/BPMP passthrough nodes, and VM1 virtio
configuration. Treat that file as the target-side configuration to preserve,
not as evidence that the lower `projects/vm` Orin platform support is present.

### Build Configuration

Keep the canonical Orin platform build entry points:

- `virtioso-build/configs/orinagx_defconfig`
- Orin Yocto layer/machine configuration for Tegra, if the image build requires
  it.

Evidence commits from `virtioso-build` history:

- `5781024 orinagx: Add Orin AGX platform defconfig`
- `36f212e orinagx: Add Yocto layer config for NVIDIA Tegra`

### seL4 Library Safety Fixes

Keep the small support fix that makes ELF reservation tolerant of a `NULL`
vspace during this bringup path:

- `projects/seL4_libs` `c8a90b0 libsel4utils: Handle NULL vspace in sel4utils_elf_reserve`

## Drop Or Do Not Replay

### Cacheability and Manual Cache-Maintenance Attempts

Do not replay changes whose purpose was to force cacheability/cache-maintenance
behavior around the old symptom. The current interpretation is that the Orin
failure was caused by improper shareability attributes and fixed in the
elfloader/seL4 mapping path.

Rejected examples:

- `projects/vm` `ded2a4e Default to clean_cache=1 on NVIDIA boards`
- `projects/vm` `83eafc8 VM_Arm: Set cacheable field in frame allocation functions`
- `kernel` `eba5a3b6e arm: Fix page table cache coherency - use dc civac (PoC) not dc cvau (PoU)`
- `kernel` `c596a2393 arm64: Fix VSpace/PageTable object creation to use PoC cache ops`
- `kernel` `a8de5828a OK: arm64: Improve Tegra cache operations and disable benchmark flush`
- `kernel` `3c4a8156b untyped: Add cache flush before clearMemory in resetUntypedCap`

The first validation attempt should run without forcing `clean_cache=1`. If the
Orin boot path still fails, especially in a way that resembles the old NVIDIA
board cache/coherency symptoms, retry with `clean_cache=1` as a bounded
experiment. Historically NVIDIA boards have needed this, and the current
shareability fixes may or may not have removed that requirement.

### RAS Error Hunting and SDEI Debugging

Do not replay RAS-hunting changes as Orin enablement. They were diagnostic
responses to the old symptom, not the target architecture.

Rejected examples:

- `kernel` `ddd498725 orinagx: Start RAM at 0x80032000 to avoid RAS errors`
- `kernel` `e16a0a667 orinagx: Add SDEI RAS error handling`
- `kernel` `f7420d87d arm64/hyp: Fix RAS errors by disabling Stage 2 during VTTBR switch`
- `kernel` `e80d11d7f arm64: Add missing DSB barriers for VTTBR writes and TLB/I-cache ops`
- `kernel` `5de8bffee EXPERIMENTAL: add isb to eret`
- `kernel` `3520dd5b7 EXPERIMENTAL: add markers indicating scheduling decisions`
- `virtioso-build` `f641cca kconfig: Auto-select KERNEL_ARM_SDEI_RAS for Orin AGX`

Related docs such as RAS implementation plans, cache-coherency stress notes, and
safe-PTE RAS narratives are historical context only unless a current failure
reproduces the same architectural condition.

### Old Trace and Hyp-Ftrace Paths

Do not replay `hyp_ftrace` or old phase-ID-heavy tracing as part of Orin bringup.
The retained tracing direction is the small shared trace framework, raw shard
transfer, and optional QEMU/kmod bridge already recorded in the rewrite plan.

If `devices.camkes` still carries `hyp_ftrace` parameters, remove or neutralize
them only after checking the active templates and generated code path.

## Defer Until Proven

These may be useful but should not be replayed in the first Orin slice:

- `projects/vm` `ea92098 orinagx: Fix MGBE conditionally`
- `projects/seL4_libs` `cc2fc4c libsel4bench: add cortex-a78 arch include`
- `virtioso-build` `5e9aa15 orinagx: Limit max CPUs to 1`
- kernel safe-invalid-PTE commits, unless they are confirmed to be the real
  upstreamable speculative page-table-walk fix rather than another local RAS
  workaround.

## Suggested Replay Order

1. Compare current `virtioso-next` content with the keep list above and mark
   which items are already present.
2. Replay or rederive missing `tools/seL4` elfloader Orin/MMU/memmap work first,
   because it is the likely dependency for the real shareability fix.
3. Replay missing `kernel` Orin platform/DTS completeness without SDEI/RAS and
   without cacheability experiments.
4. Replay missing `projects/vm` Orin platform header, 8-bit IRQ reserve, and
   optional control dataport support, including the DTB dump hook for debugging
   generated guest FDT contents.
5. Replay the `projects/seL4_libs` `sel4utils_elf_reserve()` NULL-vspace fix if
   it is not already present on the target branch.
6. Preserve the current `projects/virtioso-camkes-vm` `virtioso-next` app and
   template direction; do not overwrite newer shared-contract DT generation
   with older `orinagx-next` template code.
7. Validate first without forcing `clean_cache=1`:

```sh
make mrproper
make orinagx_defconfig
make vm_qemu_virtio
```

Then run Autopilot `test_sel4_efi` with chain `vm-qemu-virtio`.

If that fails with cache/coherency-like symptoms, run the same validation with
`clean_cache=1` as the next controlled comparison.

## Open Checks

- Runtime-test the clean build on Orin AGX with Autopilot chain
  `vm-qemu-virtio`.
- Confirm whether Orin Ethernet passthrough is required for the first
  `vm_qemu_virtio` validation, or whether console plus cross-VM virtio is enough.
- If runtime validation fails in a cache/coherency-like way, repeat with
  `clean_cache=1` as the next controlled comparison.
