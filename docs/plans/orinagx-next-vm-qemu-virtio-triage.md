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
| `kernel` | local checkout is `orinagx-next`; `virtioso-next` already has a minimal Orin platform and DTS | compare platform/DTS and avoid RAS/cache experiments |
| `tools/seL4` | `virtioso-next` exists and differs from `orinagx-next` in elfloader MMU/memmap code | likely contains the real boot/shareability fix boundary |
| `projects/virtioso-camkes-vm` | current branch is `virtioso-next`; Orin app config is already mostly present | avoid replaying older template/code over newer contract-based DT generation |
| `projects/vm` | `virtioso-next` lacks the Orin platform header; `orinagx-next` has cross-VM and Orin VM glue | likely source for missing VMM-side Orin pieces |
| `projects/seL4_libs` | has an `orinagx` branch with small support fixes | inspect only if the Orin build exposes these needs |
| `virtioso-build` | `virtioso-next` has `orinagx_defconfig`; local generated files are dirty | keep defconfig/layer facts, avoid SDEI/RAS auto-selection |

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

Evidence commits from `projects/vm/orinagx-next`:

- `97e8adf orinagx: reserve 8-bit-safe crossvm irq for vm_qemu_virtio`
- `600bf95 vm: plumb optional control dataport for crossvm`
- `8bcd8ea orinagx: Add legacy CAmkES virtio support`
- `93b11b4 orinagx: Add TCU console support via fdt_plat_customize hook`
- `cfce264 orinagx: Add PMC node and MAC address to guest DTB`
- `b37080e orinagx: Move passthrough devices from vmlinux.h to devices.camkes`
- `cdaeee1 Use existing chosen node if available`

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

Also do not add new `clean_cache` defaults as part of the Orin replay. Existing
configuration values should be revisited only with current runtime evidence.

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

- `projects/vm` `ec6d103 VM_Arm: Add DTB dump feature for debugging`
- `projects/vm` `ea92098 orinagx: Fix MGBE conditionally`
- `projects/seL4_libs` `c8a90b0 libsel4utils: Handle NULL vspace in sel4utils_elf_reserve`
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
   optional control dataport support.
5. Preserve the current `projects/virtioso-camkes-vm` `virtioso-next` app and
   template direction; do not overwrite newer shared-contract DT generation
   with older `orinagx-next` template code.
6. Validate with the canonical clean Orin flow:

```sh
make mrproper
make orinagx_defconfig
make vm_qemu_virtio
```

Then run Autopilot `test_sel4_efi` with chain `vm-qemu-virtio`.

## Open Checks

- Confirm whether current `tools/seL4/virtioso-next` already contains the exact
  elfloader fix semantics despite differing from `orinagx-next`.
- Confirm whether `kernel/virtioso-next` should keep `KernelAArch64SErrorIgnore`
  default behavior or whether the current `orinagx-next` debug setting leaked
  into the branch.
- Confirm whether Orin Ethernet passthrough is required for the first
  `vm_qemu_virtio` validation, or whether console plus cross-VM virtio is enough.
- Confirm whether existing `clean_cache=true` values in
  `apps/Arm/vm_qemu_virtio/orinagx/devices.camkes` are still active and whether
  they are merely inherited configuration or an obsolete workaround that should
  be removed in a separate evidence-backed slice.
