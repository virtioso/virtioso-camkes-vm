# tools/seL4 elfloader upstream branch plan

Date: 2026-03-15

## Summary

Create a new `tools/seL4` branch named `virtioso-next-upstream` on top of local
`master`, rebuild the existing carry patch set into a cleaned-up reordered
series, and keep a progress log in this document while the work proceeds.

The new series must:

- preserve the required AArch64 rule that cache clean/invalidate by set/way is
  not used on the relevant modern targets
- keep AArch64 cache maintenance VA-based
- restore upstream AArch64 hyp-path boot control flow where `feb4005` changed it
- remove all Orin SMMU handling from elfloader
- preserve the intended Orin AGX fan behavior and EFI memory-map diagnostics

## Required Branch and Commit Policy

- New working branch: `virtioso-next-upstream`
- Base: `tools/seL4` local `master`
- All new git commits must include:
  `Signed-off-by: Hannu Lyytinen <hannu.lyytinen@unikie.com>`
- All copyright additions or modifications introduced by new commits, except the
  BCM2711 carry commit, must use `Copyright 2026, Unikie`
- The BCM2711 carry commit keeps its original attribution unless a rebase fix
  forces a specific local copyright touch
- This document is the execution tracker and must be updated as work progresses

## Target Commit Stack

The final `virtioso-next-upstream` branch should be rebuilt as this ordered
series:

1. `elfloader-tool,bcm2711: BCM2711 support`
2. `elfloader/aarch64: keep VA-based cache maintenance and restore hyp cache-disable flow`
3. `elfloader: add Tegra Combined UART (TCU) driver`
4. `elfloader: add NVIDIA Orin AGX platform support`
5. `elfloader/orinagx: restore fan speed after ExitBootServices`
6. `elfloader/efi: print EFI memory map before ExitBootServices`

Commits to drop from the old stack:

- `a5388a9` `orinagx: Add SMMU bypass check after MMU enable`
- `5d1527b` `elfloader/orinagx: Don't modify SMMU configuration`

Commits to rewrite instead of replaying verbatim:

- `feb4005` must be replaced by a new generic AArch64 cache/boot-flow patch
- `b1dd418` and `eb04a29` must be folded into one clean fan-support patch

## Implementation Requirements

### Generic AArch64 patch

- Restore the upstream AArch64 hyp-path sequencing by reintroducing
  `disable_caches_hyp()` before `init_hyp_boot_vspace(&kernel_info)`
- Do not restore `dc cisw` or `dc isw` on AArch64
- Keep `flush_dcache_range()` or equivalent VA-based maintenance as the AArch64
  D-cache primitive
- Ensure the VA-based cache-clean coverage includes:
  - `_text.._end`
  - `_boot_pgd_down`
  - `_boot_pud_down`
  - `_boot_pmd_down`
  - `_boot_pgd_up`
  - `_boot_pud_up`
  - `_boot_pmd_up`
- Keep any explicit pre-MMU image-range cleaning needed for loaded kernel and
  user/rootserver images
- Do not leave any post-rewrite hybrid state where caches stay enabled during
  hyp boot-page-table setup

### Tegra TCU patch

- Carry forward Tegra Combined UART support after any mechanical rebase fixes
- Keep this patch Tegra-generic and separate from Orin AGX platform setup

### Orin AGX platform patch

- Limit this patch to Orin AGX platform registration and base configuration
- Do not include fan handling or SMMU handling here

### Orin AGX fan patch

- Fold the old fan support and fan-restore follow-up into one coherent patch
- Keep pre-UEFI PWM capture and post-`ExitBootServices()` restoration behavior
- Keep any BPMP/PWM steps required for the intended fan result
- Remove all SMMU-related code and comments from Orin files

### EFI memory-map patch

- Keep this patch limited to EFI memory-map printing before
  `ExitBootServices()`
- Do not mix EFI diagnostics with unrelated platform or cache changes

## Explicit Removal Requirements

- Remove the post-MMU Orin hook from `elfloader-tool/src/arch-arm/sys_boot.c`
- Remove `orinagx_smmu_disable()` and all related definitions and helpers from
  `elfloader-tool/src/plat/orinagx/platform_init.c`
- Remove comments that describe elfloader SMMU reads, writes, or post-MMU
  device-MMIO access
- Ensure no SMMU-related print output remains in the final branch

## Verification Plan

Build and test only after the branch has been reconstructed.

Build sequence from workspace root:

1. `make mrproper`
2. `make orinagx_defconfig`
3. `make vm_qemu_virtio`

Test submission:

- MCP `test_sel4_efi`
- `profile="vm-qemu-virtio"`
- `autopilot_dir="/home/hlyytine/tii-sel4/autopilot"`

Verification targets:

- AArch64 hyp boot reaches MMU enable and kernel handoff cleanly
- no SMMU-related code path or output remains
- fan restore still works after `ExitBootServices()`
- EFI memory-map print still appears before `ExitBootServices()`
- no regression due to incomplete VA-based cache cleaning

Mandatory code audit before test submission:

- confirm no AArch64 set/way cache ops remain
- confirm `_boot_pmd_down` is included in the VA-based cache-clean path
- confirm `sys_boot.c` no longer has any post-MMU Orin hook

## Progress Tracker

Status legend:

- `[ ]` not started
- `[~]` in progress
- `[x]` completed

Current status:

- `[x]` Plan written to disk and designated as the execution tracker
- `[x]` Verify `tools/seL4` repo state is suitable for branch reconstruction
- `[x]` Create branch `virtioso-next-upstream` from local `master`
- `[x]` Rebuild BCM2711 carry patch as commit 1
- `[x]` Rebuild generic AArch64 VA-cache/hyp-flow patch as commit 2
- `[x]` Rebuild Tegra TCU patch as commit 3
- `[x]` Rebuild Orin AGX platform patch as commit 4
- `[x]` Rebuild Orin AGX fan patch as commit 5
- `[x]` Rebuild EFI memory-map patch as commit 6
- `[x]` Verify commit messages, sign-offs, and copyright headers
- `[~]` Run clean Orin AGX build
- `[ ]` Run Orin AGX `vm_qemu_virtio` EFI test
- `[~]` Update this tracker with results and any follow-up actions

Verification notes:

- Branch stack created on `virtioso-next-upstream`:
  - `9c8bd14` `elfloader-tool,bcm2711: BCM2711 support`
  - `5dba4b9` `elfloader/aarch64: keep VA-based cache maintenance and restore hyp cache-disable flow`
  - `5fa8091` `elfloader: add Tegra Combined UART (TCU) driver`
  - `fce3559` `elfloader: add NVIDIA Orin AGX platform support`
  - `2703099` `elfloader/orinagx: restore fan speed after ExitBootServices`
  - `fe8f893` `elfloader/efi: print EFI memory map before ExitBootServices`
- Mandatory audit completed before build submission:
  - no AArch64 `dc cisw`/`dc isw` or Orin SMMU references remain under `elfloader-tool/`
  - `_boot_pmd_down` is included in the VA-based cache-clean path
  - `sys_boot.c` has no post-MMU Orin hook

## Notes

- The final branch intentionally keeps a minimal local AArch64-only deviation
  from upstream for VA-based cache maintenance.
- Everywhere else, the objective is to stay as close to upstream `master` as
  practical.
