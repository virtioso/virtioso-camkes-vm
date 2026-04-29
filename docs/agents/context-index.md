# Context Index

Purpose: help future agents recover active or recently paused work after chat
context loss.

Use this file as a routing index, not as the full work log. Each entry should
point to the primary durable note, summarize the last known state, and name the
next useful action.

When creating or substantially updating a durable plan, investigation note, or
long-running work item, update the matching entry here or add a new one.

## Recovery Workflow

For questions like "do you remember us working on X?", check this file first,
then open only the linked primary notes that match the topic.

Entry fields:

- Status: `active`, `paused`, `blocked`, or `done`
- Primary note: the first file to open
- Related notes: optional supporting files
- Last known state: current proof, decision, or failure boundary
- Next action: the most useful next bounded step
- Updated: last known update date

## Active Threads

### Making Autopilot usable for agents

- Status: active
- Primary note: [../plans/autopilot-usable-agent-interface-plan.md](../plans/autopilot-usable-agent-interface-plan.md)
- Related notes:
  - [autopilot-testing-policy.md](autopilot-testing-policy.md)
  - [build-test-runbook.md](build-test-runbook.md)
  - [repo-topology-policy.md](repo-topology-policy.md)
  - `/home/hlyytine/autopilot`
- Last known state: Autopilot policy has switched to a single agent-facing `autopilot` command in `PATH` that returns JSON. Autopilot startup clears pending and processing requests and reports startup cleanup. The command API now has structured daemon/tmux/worker status, bounded `logs --tail/--grep/--file`, `evidence`, structured required-hook failures in `get`, Orin AGX UART defaults, help without `WORKSPACE`, and JSON validation errors. Active workspace docs require `autopilot ... --json` and forbid MCP/direct queue/Python-internals fallback paths.
- Next action: exercise a fresh Orin AGX `vm_qemu_virtio` run and require final reporting from `autopilot get` plus, at most, `autopilot evidence`; then exercise the QEMU path and decide whether to add an optional `ci-tester` skill.
- Updated: 2026-04-28

### Orin AGX `vm_qemu_virtio` VM1 virtio-console stall

- Status: active
- Primary note: [../integration/orinagx-vm-qemu-virtio-crossvm-irq-analysis-2026-02-09.md](../integration/orinagx-vm-qemu-virtio-crossvm-irq-analysis-2026-02-09.md)
- Related notes:
  - [../platforms/orin-agx/investigations/vm-qemu-virtio-boot-investigation.md](../platforms/orin-agx/investigations/vm-qemu-virtio-boot-investigation.md)
  - [../plans/orinagx-next-vm-qemu-virtio-triage.md](../plans/orinagx-next-vm-qemu-virtio-triage.md)
  - [build-test-runbook.md](build-test-runbook.md#orin-agx-vm_qemu_virtio)
- Last known state: clean Orin AGX rebuilds on 2026-04-29 confirmed VM1 UARTI routing on Autopilot `tty1`: VM1 generated `stdout=/bus@0/serial@31d0000` with `console=ttyAMA0,115200n8`. Request `20260429-132803` uses DT-selected `earlycon` rather than hard-coded `earlycon=pl011,...`; Linux still prints `earlycon: pl11 at MMIO32 0x31d0000` because `arm,sbsa-uart` earlycon is implemented by the PL011 driver, and the same log confirms `31d0000.serial: ttyAMA0 ... is a SBSA`. Autopilot did run `upload_driver_vm_rootfs` before EFI upload. The uploaded driver image contains `/var/lib/virt/images/user-vm.qcow2`, the embedded qcow2 matches the deploy user image by sha256 (`077f26787adcd700b50e7f289266dbe7bf337a76a9cbff88c8d5ad98106ea80c`), and embedded `/etc/inittab` contains `AMA0:12345:respawn:/usr/sbin/ttyrun ttyAMA0 /bin/start_getty 115200 ttyAMA0 vt102`. Therefore the missing VM1 login is not stale eMMC, skipped Autopilot deployment, or missing packaged `SERIAL_CONSOLES`/inittab. Later `tty1.raw` showed VM1 reaches the getty issue banner and stalls mid-write at `Poky (Yocto Project Reference Distr`. IRQ trace run `20260429-222713` proved VM1 registers and enables IRQ 178, but no physical-arrive/inject/EOI occurs for that IRQ; local NVIDIA DTS shows real UARTI is `serial@31d0000` with `GIC_SPI 285`, i.e. GIC INTID 317, while `kernel/tools/dts/orinagx.dts` currently describes UARTI as SPI 146 / INTID 178. The UARTI getty stall is now an IRQ-number mismatch unless later evidence contradicts it. Timing run `20260429-145048` confirmed a VM1 pre-kernel VMM load outlier (`21.7 s` kernel load), but refreshed run `20260429-161305` measured VM1 kernel load at about `0.73 s` versus VM0 at `33 ms`; the pre-kernel gap is real but variable and no longer explains the large Linux boot delay alone. VM1 Linux reaches UART/PCI/virtio setup at about `1.8s` to `3.2s`, then is quiet until `cacheinfo` at `58.8s`, `virtio_blk` at `61.2s`, and `/sbin/init` at `71.7s`. In that same interval, `sel4_virt_stats:` shows real forwarded VM1 requests with `work_empty=0`, so this is not an empty workqueue spin. QEMU wait-thread A/B run `20260429-165235` enabled `CONFIG_QEMU_SEL4_RPC_WAIT_THREAD=y`; it did not improve the slowness, reproduced the `21.7 s` VM1 pre-kernel load class, and still delayed VM1 Linux from virtio PCI setup around `4.0s` to `cacheinfo` at `55.3s`.
- Next action: do not reopen stale-image or missing-getty questions unless new image or bootarg evidence contradicts this entry. For UARTI login, decide how to handle real INTID 317: either implement/verify full-width passthrough IRQ mapping for this non-PCI UART route, or explicitly approve a >255 physical IRQ outside the cross-VM 8-bit PCI INTx constraint; do not silently change `free_plat_interrupts[]`. Do not treat QEMU vmfd readiness as the primary suspect unless a future A/B contradicts `20260429-165235`. For post-kernel VM1 slowness, add rate-limited per-op/per-device QEMU/sel4 request classification for the `3s` to `61s` interval; current counters prove there are real forwarded requests but not which virtio device or MMIO operation dominates. For pre-kernel VM1 load, the source-backed suspect is now specific: VM1 allocator-pool RAM uses 4 KiB touch/cache-maintenance chunks while VM0 fixed `untyped_mmios` RAM uses 2 MiB chunks, predicting about `10829/22 = 492x` more touch/clean iterations for the same 44,352,000 byte kernel and matching the measured `472x` VM1/VM0 timing gap. The user's 2024 RPi4 recollection is backed by old libsel4vm commits `ad99b3a`/`9d3ac10` plus app commit `c47cda6`: RPi4 explicitly used a `guest_large_pages` libsel4vm contract and set `vm1.guest_large_pages = true` even in the VMSWIOTLB-era app shape. The January 2026 `camkes_get_untyped_page_bits()` guest-RAM allocation logic was local/AI-generated, not upstream, so it was replaced by the RPi4 contract in `projects/sel4_projects_libs` commit `5b87dd5`, `projects/vm` commits `3acdbcc`/`fab38d1`, and Orin VM1 opt-in commit `dec6f0f`. Clean build `make mrproper && make orinagx_defconfig && make vm_qemu_virtio` completed after `fab38d1` and produced `orinagx_vm_qemu_virtio/images/capdl-loader-image-arm-orinagx` at 54624580 bytes. An RPi4 carry-over audit found dataport frame-array mapping, BAR sizing, reservation-map hardening, vCPU naming, and level IRQ semantics already present or evolved in current branch. The separate ARM image-loader cache-maintenance bug was fixed in `projects/sel4_projects_libs` commit `7cf1a26`, so future timing should not rely on the old partial `0..4K` clean behavior for 2 MiB VM0 chunks. Treat seL4 kernel IPI slowness as low priority for this app unless affinity evidence changes: the generated image is SMP-capable, but VM0/VM1/VMM TCBs and one-vCPU guest TCBs are on affinity 0.
- Updated: 2026-04-29

### Orin AGX UARTA header console passthrough

- Status: active
- Primary note: [../platforms/orin-agx/uarta-bct/README.md](../platforms/orin-agx/uarta-bct/README.md)
- Related notes:
  - [../platforms/orin-agx/uarta-bct/tegra234-mb2-bct-scr-p3701-0000-uarta-vm.dts](../platforms/orin-agx/uarta-bct/tegra234-mb2-bct-scr-p3701-0000-uarta-vm.dts)
  - [../platforms/orin-agx/uarta-bct/jetson-agx-orin-devkit-uarta-vm.conf](../platforms/orin-agx/uarta-bct/jetson-agx-orin-devkit-uarta-vm.conf)
- Last known state: AGX Orin 40-pin header pins 8/10 are UART1 TX/RX, and local Tegra234 pinctrl/BCT evidence maps that signal to UARTA at `0x03100000`, not UARTI. VM1 passthrough generates the UARTA DTB/MMIO/IRQ correctly. With `clean_cache=true`, VM0 reaches shell and VM1 starts; the remaining failure is ATF CBB/ACI RAS at `ADDR = 0x8000000003100000`, which is first UARTA MMIO access. Stock Linux enables UARTA through BPMP clock/reset as `nvidia,tegra194-hsuart`, but VM1 earlycon uses a self-contained 8250-compatible node, so firmware/BCT must leave UARTA accessible before Linux driver probe.
- Next action: copy the two files from the primary note into `Linux_for_Tegra`, flash with the `jetson-agx-orin-devkit-uarta-vm` target, then rerun Autopilot `vm-qemu-virtio` and verify VM1 logs appear on the external serial adapter connected to header pins 8/10 without RAS. If RAS remains after flashing, extend the platform-side investigation from SCR firewall policy to UARTA clock/reset ownership.
- Updated: 2026-04-29

### Cross-arch console mux and stream routing

- Status: active
- Primary note: [../architecture/console-mux-nvidia-style-rewrite-plan-2026-04-27.md](../architecture/console-mux-nvidia-style-rewrite-plan-2026-04-27.md)
- Related notes:
  - [../architecture/console-mux-camkes-architecture-plan-2026-04-25.md](../architecture/console-mux-camkes-architecture-plan-2026-04-25.md)
  - [../architecture/console-transport-and-routing.md](../architecture/console-transport-and-routing.md)
  - [../architecture/autopilot-console-source-integration.md](../architecture/autopilot-console-source-integration.md)
  - [../architecture/console-stream-topology-diagrams.md](../architecture/console-stream-topology-diagrams.md)
  - [../integration/console-router-and-timeline-implementation-plan-2026-04-23.md](../integration/console-router-and-timeline-implementation-plan-2026-04-23.md)
  - [../integration/console-timeline-and-interactive-architecture-2026-04-23.md](../integration/console-timeline-and-interactive-architecture-2026-04-23.md)
  - [../integration/x86-vm-qemu-virtio-console-stream-inventory-2026-04-24.md](../integration/x86-vm-qemu-virtio-console-stream-inventory-2026-04-24.md)
  - [../integration/x86-vm-qemu-virtio-framed-console-transport-plan-2026-04-24.md](../integration/x86-vm-qemu-virtio-framed-console-transport-plan-2026-04-24.md)
- Last known state: the earlier `CF` / `binary_frames` direction is no longer the target architecture. The new direction is a minimal NVIDIA-style mux protocol on the seL4 side, second-UART isolation for low-level output, generated CAmkES stream enumeration, and `tcu_muxer`-style host demux. `vmm_mux_control` and `vmm_debug` must stay distinct.
- Next action: audit active-path dependencies on `CF`, `binary_frames`, and `line_prefixes`, then replace target emission and host demux around a generated stream inventory seam.
- Updated: 2026-04-27

Recovery note:

- if a future session asks whether the console/mux work already has a plan,
  open the primary note above first
- do not restart from the older framed-transport notes unless the task is
  explicitly historical or removal-related
- the key decision recorded on 2026-04-27 is that we own the full stack and
  therefore do not preserve compatibility with `CF` or `binary_frames`
- the intended target is NVIDIA-style `0xff <stream-id>` switching, not richer
  protocol framing

### `qemu_x86_64_defconfig` `vm_qemu_virtio` target topology

- Status: active
- Primary note: [../integration/x86-qemu-pc99-vm-qemu-virtio-port-notes-2026-04-16.md](../integration/x86-qemu-pc99-vm-qemu-virtio-port-notes-2026-04-16.md)
- Related notes:
  - [../integration/arm64-vm-qemu-virtio-shape.md](../integration/arm64-vm-qemu-virtio-shape.md)
  - [../architecture/autopilot-qemu-backends.md](../architecture/autopilot-qemu-backends.md)
  - [build-test-runbook.md](build-test-runbook.md)
- Last known state: the x86 app is considered leftover early-Isengard scaffolding. The target is to replicate the Arm `vm_qemu_virtio` shape on `qemu_x86_64`: two VMs, VM0 booting `vm-image-driver`, VM1 booting `vm-image-user` from within VM0, without Isengard-specific CAN/Kvaser/native PCI service logic in the x86 app itself.
- Next action: understand and preserve the physical-PCI-plus-vPCI coexistence model before optimizing wait paths or deleting apparently awkward topology.
- Updated: 2026-04-27

### QEMU backend enablement and remote runner flow

- Status: paused
- Primary note: [../architecture/autopilot-qemu-backends.md](../architecture/autopilot-qemu-backends.md)
- Related notes:
  - [../integration/qemu-backend.md](../integration/qemu-backend.md)
  - [../integration/qemu-sel4-accelerator.md](../integration/qemu-sel4-accelerator.md)
  - [../architecture/yocto-qemu-runtime-artifact-contract.md](../architecture/yocto-qemu-runtime-artifact-contract.md)
  - [../architecture/yocto-qemu-runtime-implementation-plan.md](../architecture/yocto-qemu-runtime-implementation-plan.md)
  - [../start-here/running-qemu.md](../start-here/running-qemu.md)
- Last known state: repo-side manual runners/docs are the source of truth; Autopilot should orchestrate that path rather than reimplement boot mechanics. `qemu_x86_64_defconfig` is a remote Autopilot-backed QEMU target.
- Next action: when QEMU validation fails, separate runner/transport failures from guest/runtime failures and collect queue truth from Autopilot.
- Updated: 2026-04-23

### qemuarm64 `vm_minimal` and `vm_qemu_virtio` build/boot fixes

- Status: paused
- Primary note: [../integration/arm64-vm-qemu-virtio-shape.md](../integration/arm64-vm-qemu-virtio-shape.md)
- Related notes:
  - [../integration/qemu-backend.md](../integration/qemu-backend.md)
  - [../architecture/yocto-qemu-runtime-artifact-contract.md](../architecture/yocto-qemu-runtime-artifact-contract.md)
- Last known state: workspace-root config and `ARCH_ARM64=AARCH64` mapping were central. Remaining qemuarm64 virtio boot trouble was tied to guest DT/PCI host layout; `highmem=off` was part of the intended low-vPCI design rather than the first thing to remove.
- Next action: reproduce exact top-level commands before diagnosing generated artifacts; inspect generated DT and PCI host layout before changing QEMU machine flags.
- Updated: 2026-04-16

### Isengard portable semantic core and Linux-only QEMU path

- Status: active
- Primary note: [../../../isengard-camkes-vm/docs/linux-only-qemu-migration-plan.md](../../../isengard-camkes-vm/docs/linux-only-qemu-migration-plan.md)
- Related notes:
  - [../../../isengard-camkes-vm/docs/architecture.md](../../../isengard-camkes-vm/docs/architecture.md)
  - [../../../isengard-camkes-vm/docs/README.md](../../../isengard-camkes-vm/docs/README.md)
- Last known state: business logic must exist in one shared semantic implementation and remain freely placeable in native seL4, native Linux, or Linux-on-seL4. `projects/isengard-camkes-vm` is seL4-side only; host-portable implementation belongs under `sources/`, currently `sources/isengard-core`, with new Linux-side image work routed through `vm-images/meta-isengard`.
- Next action: for "continue Isengard" requests, start from the primary migration plan and keep new deployment-neutral contracts out of CAmkES IDL.
- Updated: 2026-04-23

### Legacy Autopilot MCP startup, status, and queue truth

- Status: paused
- Primary note: [autopilot-testing-policy.md](autopilot-testing-policy.md)
- Related notes:
  - [build-test-runbook.md](build-test-runbook.md)
  - [repo-topology-policy.md](repo-topology-policy.md)
  - `/home/hlyytine/autopilot/sel4_mcp_server.py`
  - `/home/hlyytine/autopilot/sel4_mcp_server_wrapper.sh`
- Last known state: `WORKSPACE=/home/hlyytine/tii-sel4` and `AUTOPILOT_DIR=/home/hlyytine/tii-sel4/autopilot` are required. Some failures were startup/env issues, some were Codex live-session binding issues, and one compatibility fix required newline-delimited JSON on stdout rather than `Content-Length` framing.
- Next action: preserve this entry only as historical MCP troubleshooting context. Do not use MCP as the active test workflow unless the single-API policy is explicitly changed again.
- Updated: 2026-04-23

### Yocto VM image and local-source workflow

- Status: paused
- Primary note: [build-test-runbook.md#orin-agx-kmod-sel4-virt-yocto-module-recipe](build-test-runbook.md#orin-agx-kmod-sel4-virt-yocto-module-recipe)
- Related notes:
  - [example-workflows-fastpath.md#yocto-vm-image-fast-path](example-workflows-fastpath.md#yocto-vm-image-fast-path)
  - [../plans/kmod-sel4-virt-virtioso-next-rewrite-plan.md](../plans/kmod-sel4-virt-virtioso-next-rewrite-plan.md)
- Last known state: active Yocto layer is `vm-images/virtioso-yocto-layers/meta-virtioso-sel4/`. Yocto-managed source edits belong only in `sources/qemu`, `sources/kmod-sel4-virt`, and `sources/sel4-linux-kernel-support`, and those repos must be clean before bitbake.
- Next action: for VM image work, scope first to touched recipes/source paths and stale `vm-images/build/workspace` appends before re-studying whole layers.
- Updated: 2026-04-27

### Manifest-level `kmod-sel4-virt` / QEMU / contracts rewrite on `virtioso-next`

- Status: active
- Primary note: [../plans/kmod-sel4-virt-virtioso-next-rewrite-plan.md](../plans/kmod-sel4-virt-virtioso-next-rewrite-plan.md)
- Related notes:
  - [../integration/kmod-sel4-virt-backend-analysis.md](../integration/kmod-sel4-virt-backend-analysis.md)
  - [../architecture/cross-el-tracing-feasibility.md](../architecture/cross-el-tracing-feasibility.md)
  - [../plans/orinagx-next-vm-qemu-virtio-triage.md](../plans/orinagx-next-vm-qemu-virtio-triage.md)
  - [../plans/ftrace-upstream-integration-plan.md](../plans/ftrace-upstream-integration-plan.md)
  - [build-test-runbook.md#orin-agx-kmod-sel4-virt-yocto-module-recipe](build-test-runbook.md#orin-agx-kmod-sel4-virt-yocto-module-recipe)
- Last known state: `sources/kmod-sel4-virt` post-`virtioso-next` work was grouped into replay topics, then expanded into a manifest-level rewrite. Connected repos include `sources/qemu`, `sources/virtioso-contracts`, `sources/kmod-vio-trace`, `projects/virtioso-camkes-vm`, `vm-images/virtioso-yocto-layers`, `sources/sel4-linux-kernel-support`, and selected `projects/sel4_projects_libs` support. Current direction: first extract headers/protocol definitions already common to kmod, QEMU, and the seL4 VMM into `sources/virtioso-contracts` so separate copies are eliminated before feature replay. `sources/virtioso-contracts` now starts with `6985775 rpc: bootstrap shared contract headers`; use `backup-virtioso-next-before-contracts-first-rewrite` as the local source branch for later contract replay. `sources/kmod-sel4-virt` has been switched to `virtioso-next` for the wrapper/include-path slice. Important correction remains: later `virtioso-contracts` commits must not be landed as one blob; direct MMIO slots, backend/control mailbox, RPC ordering/cache helpers, and trace phase IDs travel with their consuming topic.
- Next action: the single-device event/data/control BAR slice has been replayed
  with `sources/virtioso-contracts` commit `eeb8a36` and
  `sources/kmod-sel4-virt` commit `cb128e9`. The DT discovery/backend-choice
  slice has contracts commit `f91ac6e` and kmod commit `70ebe5a`; the VMM
  template emits `virtioso,sel4-camkes-rpc` DT nodes from the shared contracts.
  A VMM FDT-generation correctness follow-up now keeps walking `_fdt_node`
  entries after one generated node; do not replay the old event-BAR cacheability
  change because the Orin AGX issue was shareability, not event-BAR caching.
  No current QEMU source change has been found for these slices. RPC/cache-sync
  workarounds are dropped unless a new non-shareability bug is proven.
  Generation-based backend mailbox and direct-MMIO-slot rewrites are rejected.
  The next retained topic is trace framework extraction: minimal trace
  record/source/shard contracts, `kmod-vio-trace`, DT shard wiring, and tooling,
  without bulk replay of old cache/mailbox/MMIO-slot phase IDs. Keep broad
  platform/debug history out unless a commit is contract-critical.
- Trace framework extraction has started: `sources/virtioso-contracts` commit
  `8d16eb9` defines the minimal trace framework contract, and
  `sources/kmod-vio-trace` commit `f6f834c` consumes it. No event or phase IDs
  were added. The VMM trace DT generator now uses the shared contract constants
  for the `vio_trace` reserved-memory node and shard identity properties.
  `sources/virtioso-contracts` commit `899149d` adds the raw
  `VIO_TRACE_STREAM` transfer contract; `tools/vio-trace` now extracts raw
  buffers and manifests without decoding obsolete event/phase IDs.
  `vm-images/virtioso-yocto-layers` now has a retained trace image-integration
  slice: `virtioso-contracts` packages the current backend/RPC/trace headers,
  the trace module recipe consumes local `sources/kmod-vio-trace` plus staged
  contracts, `vm-image-driver` installs `kernel-module-vio-trace`, and the
  local-source cleanliness guard includes both `virtioso-contracts` and
  `kmod-vio-trace`. The trace bridge/QEMU slice is now in progress:
  `virtioso-contracts` owns the shard-open request shape,
  `sources/kmod-sel4-virt` exposes `SEL4_TRACE_OPEN_SHARD` by delegating to
  `kmod-vio-trace`, and `sources/qemu` opens/maps an EL0 shard when available
  without adding event IDs. A narrow VM-fd poll slice follows this: kmod makes
  the existing VM fd readable when the forwarded RPC queue has work, and QEMU
  drains that existing queue from `qemu_set_fd_handler()` instead of a wait
  thread. Do not add old `SEL4_DELEG_*`, direct slots, mailbox, or generation
  fields.
- Orin AGX branch triage is now recorded separately. Keep real Orin platform,
  DTS, carefully sliced elfloader work, cross-VM IRQ, control-dataport support,
  the VM DTB dump hook, and the `sel4utils_elf_reserve()` NULL-vspace fix. Do
  not replay the old cacheability attempts, SDEI/RAS-hunting changes, or
  hyp-ftrace path. Try without forced `clean_cache=1` first, but retain it as a
  fallback experiment if symptoms justify it.
- Updated: 2026-04-27

### Cross-EL tracing implementation

- Status: paused
- Primary note: [../architecture/cross-el-tracing-feasibility.md](../architecture/cross-el-tracing-feasibility.md)
- Related notes:
  - [../plans/ftrace-upstream-integration-plan.md](../plans/ftrace-upstream-integration-plan.md)
- Last known state: tracing implementation has strict change control. Repos to be touched must be clean, work must happen on a dedicated branch, and commits should reference the design source.
- Next action: before any tracing code edits, verify repo cleanliness and request explicit approval for branch creation or dirty-repo handling.
- Updated: 2026-04-27

### CANopen snapshot publication from seL4 to Linux

- Status: paused
- Primary note: [../../../../normet/docs/can-stack-architecture.md](../../../../normet/docs/can-stack-architecture.md)
- Related notes:
  - [../architecture/memory-model.md](../architecture/memory-model.md)
  - [../architecture/virtio-architecture.md](../architecture/virtio-architecture.md)
- Last known state: preferred direction was coherent shared-memory snapshots from native seL4-owned CANopen state to a Linux guest, rather than treating virtio as the default answer for application-state sharing.
- Next action: if resumed, verify the exact `normet/docs` tree in this workspace before editing and keep the snapshot consistency requirement central.
- Updated: 2026-04-18

### Normet rugged x86 secure boot, TPM, attestation, and BSP questions

- Status: active
- Primary note: [../../../../../normet/docs/05-platform-next/trust-and-update/Rugged_x86_Secure_Boot_TPM_Attestation_And_BSP_Questions.md](../../../../../normet/docs/05-platform-next/trust-and-update/Rugged_x86_Secure_Boot_TPM_Attestation_And_BSP_Questions.md)
- Related notes:
  - [../../../../../normet/docs/05-platform-next/trust-and-update/Boot_Trust_And_Platform_Ownership_Comparison.md](../../../../../normet/docs/05-platform-next/trust-and-update/Boot_Trust_And_Platform_Ownership_Comparison.md)
  - [../../../../../normet/docs/05-platform-next/trust-and-update/Why_Update_And_Boot_Trust_Must_Be_First_Class_In_Normet_Platform_Design.md](../../../../../normet/docs/05-platform-next/trust-and-update/Why_Update_And_Boot_Trust_Must_Be_First_Class_In_Normet_Platform_Design.md)
- Last known state: EPEC rugged `x86` call-prep requirements were captured in a durable Normet note. The key questions are customer-owned `UEFI` Secure Boot `PK`/`KEK`/`db`/`dbx`, `TPM 2.0` type and provisioning, measured boot and attestation behavior, BSP source availability, arbitrary customer-signed EFI payloads, custom Linux or hypervisor/seL4 loader support, `VT-x`/`VT-d`/IOMMU exposure, update/recovery semantics, and the minimum engineering package to request from the vendor.
- Next action: after the EPEC call, update the primary note with vendor answers and classify each answer as acceptable, risky, blocked, or requiring follow-up evidence.
- Updated: 2026-04-27

### PX4 uORB access model

- Status: paused
- Primary note: this entry; no dedicated repo note exists yet.
- Related notes:
  - PX4 uORB documentation: `https://docs.px4.io/main/en/middleware/uorb`
  - PX4 uORB manager source/API references for the `px4_open`/`px4_read` backing model.
- Last known state: PX4 uORB topics are exposed internally as file-like virtual device nodes, commonly visible under `/obj`, and the implementation uses file-descriptor style operations such as open/read/ioctl/poll. Normal application code should still be described as using the uORB pub/sub API (`orb_advertise`, `orb_publish`, `orb_subscribe`, `orb_copy`, or C++ wrappers), not as reading and writing ordinary persistent files.
- Next action: if this becomes part of a platform architecture comparison, create a dedicated architecture note and keep the distinction between virtual device-node backing and application-level pub/sub semantics explicit.
- Updated: 2026-04-27

### Novice AI usage guidance

- Status: paused
- Primary note: `ohjeet2.md` if present in the workspace root
- Related notes:
  - `ohjeet.md` if present in the workspace root
- Last known state: `ohjeet2.md` was written as the improved beginner-facing replacement while preserving the original. The useful sections included concrete prompt examples, on-disk instruction files, planning before edits, verification requests, MCP, context management, and tool/vendor caveats.
- Next action: if revising, keep it beginner-friendly and explicit about differences between Claude, OpenAI, browser chat, and local coding-agent behavior.
- Updated: 2026-04-24

## Completed Or Historical Threads Worth Remembering

### Orin AGX platform porting and low-level plans

- Status: paused
- Primary note: [../platforms/orin-agx/porting/orinagx-camkes-porting.md](../platforms/orin-agx/porting/orinagx-camkes-porting.md)
- Related notes:
  - [../platforms/orin-agx/porting/vm-qemu-virtio-orinagx.md](../platforms/orin-agx/porting/vm-qemu-virtio-orinagx.md)
  - [../platforms/orin-agx/plans/gicv3-vgic-implementation-plan.md](../platforms/orin-agx/plans/gicv3-vgic-implementation-plan.md)
  - [../platforms/orin-agx/plans/orin-agx-smmu-implementation-plan.md](../platforms/orin-agx/plans/orin-agx-smmu-implementation-plan.md)
  - [../platforms/orin-agx/plans/ras-errors-implementation-plan.md](../platforms/orin-agx/plans/ras-errors-implementation-plan.md)
  - [../platforms/orin-agx/plans/stage2-device-cacheability-fix-plan.md](../platforms/orin-agx/plans/stage2-device-cacheability-fix-plan.md)
- Last known state: useful historical context for Orin AGX platform work, but not command authority for routine build/test tasks.
- Next action: use only when a platform-porting question specifically needs the old technical context.
- Updated: 2026-04-27

### VM image boot unification and CAmkES-to-Microkit planning

- Status: paused
- Primary note: [../plans/vm-image-boot-unification-plan.md](../plans/vm-image-boot-unification-plan.md)
- Related notes:
  - [../plans/camkes-to-microkit-migration.md](../plans/camkes-to-microkit-migration.md)
  - [../plans/dtb-generation-enhancements-plan.md](../plans/dtb-generation-enhancements-plan.md)
  - [../plans/capdl-autopilot-extension-plan.md](../plans/capdl-autopilot-extension-plan.md)
- Last known state: planning material for future architecture/migration work; do not treat as overriding current AGENTS/runbook policy.
- Next action: open when the user asks about long-term VM boot, DT generation, CapDL automation, or CAmkES-to-Microkit migration.
- Updated: 2026-04-27
