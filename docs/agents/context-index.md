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

### Unikie AI Boost Autopilot competition entry

- Status: paused
- Primary note: `/home/hlyytine/aicomp/entry3.md`
- Related notes:
  - `/home/hlyytine/aicomp/autopilot-impact-inventory-internal.md`
  - `/home/hlyytine/aicomp/autopilot-demo-video-plan.md`
  - `/home/hlyytine/aicomp/entry2.md`
  - `/home/hlyytine/aicomp/Unikie AI Boost - Unikie.pdf`
  - `/home/hlyytine/aicomp/Unikie AI Boost Reward.pdf`
  - `/home/hlyytine/autopilot`
- Last known state: Unikie has launched an internal AI competition, and Autopilot is considered worth submitting. The draft application files already exist under `/home/hlyytine/aicomp`. `entry3.md` is the tighter current pitch; `entry2.md` is the longer, more complete source draft. The internal impact inventory now captures the stronger before/after framing, pKVM and Isengard time estimates, and concrete outcomes: Orin AGX pKVM SMMUv2 driver validation, broader seL4/Virtioso hardware/QEMU validation, and anonymized industrial-controller platform architecture/prototype work. Preserve the fact that Autopilot itself was written by AI agents and is used by AI agents to test low-level systems on real hardware. The current demo-video candidate is an ordinary Codex prompt asking Autopilot to run Orin AGX `vm_qemu_virtio`, log into VM1, inspect `/proc/cpuinfo`, and cite the request/result evidence; needed demo polish should stay generic, especially tmux panes created from runtime introspection rather than hardcoded VM names.
- Next action: turn the internal inventory into a max-two-page application under the official headings: situation, AI approach, result, contrast, and evidence. Keep the "before vs after" section central, quantify where possible, anonymize the industrial-controller project unless disclosure is explicitly allowed, and decide whether to implement the generic Autopilot tmux-pane enhancements before recording the VM1 CPU-info demo.
- Updated: 2026-05-03

### Orin AGX `vm_qemu_virtio` VM1 virtio-console stall

- Status: done
- Primary note: [../integration/orinagx-vm-qemu-virtio-crossvm-irq-analysis-2026-02-09.md](../integration/orinagx-vm-qemu-virtio-crossvm-irq-analysis-2026-02-09.md)
- Related notes:
  - [../platforms/orin-agx/investigations/vm-qemu-virtio-boot-investigation.md](../platforms/orin-agx/investigations/vm-qemu-virtio-boot-investigation.md)
  - [../plans/orinagx-next-vm-qemu-virtio-triage.md](../plans/orinagx-next-vm-qemu-virtio-triage.md)
  - [build-test-runbook.md](build-test-runbook.md#orin-agx-vm_qemu_virtio)
- Last known state: clean Orin AGX rebuilds on 2026-04-29 confirmed VM1 UARTI routing on Autopilot `tty1`: VM1 generated `stdout=/bus@0/serial@31d0000` with `console=ttyAMA0,115200n8`. Earlier image/getty checks proved the missing VM1 login was not stale eMMC, skipped Autopilot deployment, or missing packaged `SERIAL_CONSOLES`/inittab. IRQ trace run `20260429-222713` proved VM1 registered and enabled IRQ 178, but no physical-arrive/inject/EOI occurred for that IRQ; local NVIDIA DTS shows real UARTI is `serial@31d0000` with `GIC_SPI 285`, i.e. GIC INTID 317. The direct DT passthrough IRQ path was verified as full-width and separate from the 8-bit PCI/cross-VM `interrupt_line` path. Commits `2beea25a4` (kernel), `19dfd87` (projects/vm), and `3cdd26f` (virtioso-camkes-vm) corrected UARTI to SPI 285 / INTID 317. Validation run `20260429-224104` proved `route-register`, `physical-arrive`, `inject-enter`, `load-lr`, `eoi`, and `physical-ack` for IRQ 317, and VM1 reached `user-vm login:`; manual login to `root@user-vm` worked. The chain still reported fail because `wait_qemu_bootstrap_diag` timed out after `wait_uservm_tty1_ready` had already passed, and the analysis hook still reports `virtio_console_probe_window_check: fail`. Timing run `20260429-145048` confirmed a VM1 pre-kernel VMM load outlier (`21.7 s` kernel load), but refreshed run `20260429-161305` measured VM1 kernel load at about `0.73 s` versus VM0 at `33 ms`; the pre-kernel gap is real but variable and no longer explains the large Linux boot delay alone. VM1 Linux reaches UART/PCI/virtio setup at about `1.8s` to `3.2s`, then is quiet until `cacheinfo` at `58.8s`, `virtio_blk` at `61.2s`, and `/sbin/init` at `71.7s`. In that same interval, `sel4_virt_stats:` shows real forwarded VM1 requests with `work_empty=0`, so this is not an empty workqueue spin. QEMU wait-thread A/B run `20260429-165235` enabled `CONFIG_QEMU_SEL4_RPC_WAIT_THREAD=y`; it did not improve the slowness, reproduced the `21.7 s` VM1 pre-kernel load class, and still delayed VM1 Linux from virtio PCI setup around `4.0s` to `cacheinfo` at `55.3s`.
- Last known state: Autopilot run `20260504-223718` returned `overall_status: pass`. VM1 reached `user-vm login:`, auto-logged in as `root`, and confirmed `uname -a` on aarch64. All prior failures (`wait_qemu_bootstrap_diag` timeout, `virtio_console_probe_window_check: fail`) are gone. UARTI IRQ fix (SPI 285/INTID 317), `guest_large_pages` allocation fix, and ARM image-loader cache-maintenance fix are all confirmed working together.
- Next action: none — treat as closed. Reopen only if a future clean build regresses.
- Updated: 2026-05-04

### Orin AGX UARTA header console passthrough

- Status: paused
- Primary note: [../platforms/orin-agx/uarta-bct/README.md](../platforms/orin-agx/uarta-bct/README.md)
- Related notes:
  - [../platforms/orin-agx/uarta-bct/tegra234-mb2-bct-scr-p3701-0000-uarta-vm.dts](../platforms/orin-agx/uarta-bct/tegra234-mb2-bct-scr-p3701-0000-uarta-vm.dts)
  - [../platforms/orin-agx/uarta-bct/jetson-agx-orin-devkit-uarta-vm.conf](../platforms/orin-agx/uarta-bct/jetson-agx-orin-devkit-uarta-vm.conf)
- Last known state: AGX Orin 40-pin header pins 8/10 are UART1 TX/RX, and local Tegra234 pinctrl/BCT evidence maps that signal to UARTA at `0x03100000`, not UARTI. VM1 passthrough generates the UARTA DTB/MMIO/IRQ correctly. With `clean_cache=true`, VM0 reaches shell and VM1 starts; the remaining failure is ATF CBB/ACI RAS at `ADDR = 0x8000000003100000`, which is first UARTA MMIO access. Stock Linux enables UARTA through BPMP clock/reset as `nvidia,tegra194-hsuart`, but VM1 earlycon uses a self-contained 8250-compatible node, so firmware/BCT must leave UARTA accessible before Linux driver probe.
- Next action: copy the two files from the primary note into `Linux_for_Tegra`, flash with the `jetson-agx-orin-devkit-uarta-vm` target, then rerun Autopilot `vm-qemu-virtio` and verify VM1 logs appear on the external serial adapter connected to header pins 8/10 without RAS. If RAS remains after flashing, extend the platform-side investigation from SCR firewall policy to UARTA clock/reset ownership.
- Updated: 2026-04-29

### Cross-arch TCU-style UART mux and stream routing

- Status: active
- Primary note: [../architecture/cross-arch-tcu-uart-mux-plan-2026-05-01.md](../architecture/cross-arch-tcu-uart-mux-plan-2026-05-01.md)
- Related notes:
  - [../architecture/orin-uarti-mux-carrier-plan-2026-05-03.md](../architecture/orin-uarti-mux-carrier-plan-2026-05-03.md)
  - [../architecture/console-mux-nvidia-style-rewrite-plan-2026-04-27.md](../architecture/console-mux-nvidia-style-rewrite-plan-2026-04-27.md)
  - [../architecture/console-mux-camkes-architecture-plan-2026-04-25.md](../architecture/console-mux-camkes-architecture-plan-2026-04-25.md)
  - [../architecture/console-transport-and-routing.md](../architecture/console-transport-and-routing.md)
  - [../architecture/autopilot-console-source-integration.md](../architecture/autopilot-console-source-integration.md)
  - [../architecture/console-stream-topology-diagrams.md](../architecture/console-stream-topology-diagrams.md)
  - [../integration/console-router-and-timeline-implementation-plan-2026-04-23.md](../integration/console-router-and-timeline-implementation-plan-2026-04-23.md)
  - [../integration/console-timeline-and-interactive-architecture-2026-04-23.md](../integration/console-timeline-and-interactive-architecture-2026-04-23.md)
  - [../integration/x86-vm-qemu-virtio-console-stream-inventory-2026-04-24.md](../integration/x86-vm-qemu-virtio-console-stream-inventory-2026-04-24.md)
  - [../integration/x86-vm-qemu-virtio-framed-console-transport-plan-2026-04-24.md](../integration/x86-vm-qemu-virtio-framed-console-transport-plan-2026-04-24.md)
  - [../integration/orinagx-vm-qemu-virtio-crossvm-irq-analysis-2026-02-09.md](../integration/orinagx-vm-qemu-virtio-crossvm-irq-analysis-2026-02-09.md)
  - [../platforms/orin-agx/investigations/tcu-console-sporadic-hang.md](../platforms/orin-agx/investigations/tcu-console-sporadic-hang.md)
- Last known state: the earlier `CF` / `binary_frames` direction is no longer the target architecture and does not need compatibility preservation. The active completion target is now real Orin AGX CAmkES component mux/demux; x86 mux completion is backlog/regression unless a shared fix is needed for Orin. The shared architecture still uses a minimal NVIDIA TCU-style mux contract across platforms with `0xfe <stream-id>` as our escape/switch sequence instead of NVIDIA `tcu_muxer`'s `0xff` so our mux can run over a real NVIDIA TCU path. Template-generated identity comes first, and all manually assigned stream IDs are migration targets to remove. Every CAmkES component should get generated identity metadata, but only UART-like components get valid stream IDs; non-console components report an invalid value such as `-1`, and there is no shared default stream. Initial eligibility should be `PutChar`/`GetChar`; `Batch` may need inclusion later for mux-specific batch endpoints, but must not be globally treated as console-bearing without an explicit annotation or connector rule. Guest-facing UART sinks/device models should coalesce TX like a 16450/16550-style path: newline or carriage return flushes immediately, capacity flushes immediately, and partial lines flush via a configurable delayed timer with `50 ms` as the initial default. The mux announces generated stream IDs, CAmkES component names/types, directions, and aliases to the demuxer, and the demuxer creates PTYs/logs from that introspection instead of hard-coded maps. Autopilot must consume the same introspection and stop embedding stream IDs or post-demux `/dev` assumptions. The 2026-05-03 Orin direction changes UART ownership: `ttyACM0`/TCU remains raw CCPLEX boot and recovery, while `ttyACM1`/UARTI becomes the dedicated mux carrier owned by native `ConsoleMux`; VM1 no longer owns UARTI passthrough and should use generated logical streams through an emulated PL011 like VM0. Host `tcu_muxer` must be started by the per-run Autopilot chain after raw CCPLEX reaches elfloader, not by a daemon-wide wrapper, so `tcu_muxer_logs/*`, `sessions.json`, and the stream registry are owned by the current result directory.
- Last known state: Orin AGX mux/demux is confirmed end-to-end working by Autopilot run `20260504-223718` (chain: pass). `sessions.json` for that run shows all 9 sessions created from live runtime announcements — no hard-coded maps. Component types (`VM0`, `VM1`, `GuestConsoleSink`, `ConsoleMux`, `FileServer`), directions, and generated stream IDs (2–5) are all correct. `fserv` correctly gets `stream_id=-1` and no PTY. Both `vm0_guest_console_sink.txt` and `vm1_guest_console_sink.txt` have real content; VM1 reached `user-vm login:` and ran `uname -a`. The earlier `20260503-093554` login failure is resolved. x86 mux/demux remains blocked by an unrelated EPT fault (`X86EPTPageMap: Need a page directory first.`) that stops the x86 run before live stream announcements; do not fix that fault merely to finish mux/demux.
- Next action: Orin AGX is done. x86 mux/demux is backlog — unblock only if the EPT fault is fixed for independent reasons or a shared Orin/x86 fix is needed.
- Updated: 2026-05-04

Recovery note:

- if a future session asks whether the console/mux work already has a plan, or
  asks about making NVIDIA TCU-style muxing available on both x86 and Arm, open
  the primary note above first
- do not restart from the older framed-transport notes unless the task is
  explicitly historical or removal-related; `line_prefixes`, `binary_frames`,
  and `CF` framing are local-only post-upstream mux/demux work and may be
  rewritten instead of preserved
- the key decision recorded on 2026-04-27 is that we own the full stack and
  therefore do not preserve compatibility with `CF` or `binary_frames`
- the intended target is NVIDIA-style stream switching with our own `0xfe`
  escape byte, not NVIDIA's `0xff`, and not richer protocol framing
- do not resume from a plan that keeps existing numeric stream IDs "for now";
  common CAmkES-generated identity and introspection are the first required slice
- `Batch` is deferred: it may need to become stream-eligible for mux-specific
  endpoints later, but first eligibility should be `PutChar`/`GetChar`
- do not bake Orin TCU, UARTI, SBSA UART, PL011, 8250, or QEMU chardev behavior
  into the mux core; use selected platform UART backends
- x86 mux completion is backlog/regression; do not let x86-specific VM control
  faults block the Orin AGX mux/demux path

### `qemu_x86_64_defconfig` `vm_qemu_virtio` target topology

- Status: paused
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

### Isengard platform work

- Status: active
- Primary note: [../../../isengard-camkes-vm/docs/isengard-master-tracker.md](../../../isengard-camkes-vm/docs/isengard-master-tracker.md)
- Related notes:
  - [../../../isengard-camkes-vm/docs/normet-isengard-project-direction.md](../../../isengard-camkes-vm/docs/normet-isengard-project-direction.md)
  - [../../../isengard-camkes-vm/docs/orin-agx-support-plan.md](../../../isengard-camkes-vm/docs/orin-agx-support-plan.md)
  - [../../../isengard-camkes-vm/docs/orin-can-simulator-vm-architecture.md](../../../isengard-camkes-vm/docs/orin-can-simulator-vm-architecture.md)
  - [../../../isengard-camkes-vm/docs/linux-only-qemu-migration-plan.md](../../../isengard-camkes-vm/docs/linux-only-qemu-migration-plan.md)
  - [../../../isengard-camkes-vm/docs/architecture.md](../../../isengard-camkes-vm/docs/architecture.md)
- Last known state: Isengard end-to-end demo on Orin AGX bare-metal is **done** — confirmed by Autopilot run `20260505-135944`. Full pipeline: stock Linux boot → SSH → Isengard rootfs upload → EFI boot → DHCP/SSH into Isengard → `isengard-demo-start` (vcan0 + isengard_app + FUSE /obj via snapshot_memfd_demo) → verify `/obj/obj/isengard/runtime/sequence` and `can/status_word`. Key fixes landed: busybox ifup udhcpc script path, $subnet vs $mask in 50default, SSH options in autopilot chain_runtime.py, snapshot_memfd_demo installed and wired correctly to isengard_objfs. Phase 0 (CAN contract and evidence) and Phase 1 (connect isengard_app CAN data to snapshot region) are the next active targets.
- Next action: open `isengard-master-tracker.md` to pick up Phase 0/Phase 1. The Linux-native demo substrate is solid; next slice is wiring real `isengard_app` CANopenNODE data into the snapshot provider so `/obj/obj/isengard/` reflects live CAN state rather than memfd demo values.
- Updated: 2026-05-05

### virtioso-muxd Linux stream multiplexer and isengard zenoh demo

- Status: active
- Primary note: `sources/virtioso-muxd/` (source) and `autopilot/chains/isengard-linux-orin-zenoh-demo.json` (chain)
- Related notes:
  - `sources/isengard-core/meta-isengard/recipes-isengard/isengard-app/files/isengard-demo-zenoh-remote` (demo script)
  - `sources/virtioso-muxd/virtioso-mux-exec/src/main.rs` (mux-exec client)
  - `vm-images/virtioso-yocto-layers/meta-virtioso-sel4/recipes-devtools/virtioso-muxd/virtioso-muxd_git.bb` (Yocto recipe)
  - `sources/isengard-core/meta-isengard/images/isengard-image-native.bb` (image recipe)
- Last known state (2026-05-07):
  - `virtioso-mux-exec` pipe-hang fix is committed: child-exit oneshot channel + 2s drain timeout prevents infinite wait when grandchild inherits pipe fd.
  - `isengard-demo-zenoh-remote` zenoh session polling is fixed: replaced `sleep 1` with a 30×0.5s poll loop checking `bridge: zenoh session open` in the bridge log before proceeding to the subscriber step.
  - Autopilot chain `isengard-linux-orin-zenoh-demo` is live with: map_window steps (tty0=Isengard Console, tty1=Mux Output), `start_muxd` sinking to `uart:/dev/ttyAMA0`, and `run_zenoh_demo` wrapping demo in `sh -c 'isengard-demo-zenoh-remote 2>&1 | tee /tmp/demo.log'`.
  - Run `20260507-104410` reported overall_status=pass, BUT `run_zenoh_demo` completed in only 2.5s — too fast for zenoh session establishment. Root cause: `sh -c 'demo 2>&1 | tee file'` exits 0 even when demo exits 1 (POSIX sh pipeline exit status = last command = tee), so the chain always passes regardless of demo outcome. The `;true` at the end of the chain cmd compounds this.
  - muxd IS producing `0xfe` frames to `ttyAMA0` — confirmed in tty1.jsonl: `\xfe\x01\x1a\x00==> Starting zenoh bridge\r\n` and `bridge: zenoh session open` visible in the log at ~6.3MB offset (written after the chain completed, likely from board still running). ttyAMA0 is also the isengard kernel console, so tty1 shows mixed kernel dmesg + mux frames.
  - Device mapping: ttyAMA0 inside isengard = ttyACM1 on autopilot host = UARTI at MMIO 0x31d0000. Confirmed from isengard kernel boot log.
- Next action:
  1. Fix the false-positive: change chain `run_zenoh_demo` to use `set -o pipefail` or capture demo exit code explicitly, OR add a `check_demo_log` step that greps `/tmp/demo.log` for `==> PASS` and fails if absent.
  2. Rerun the chain to confirm the zenoh demo actually passes end-to-end (zenoh session opens, subscriber collects 10 samples, PASS marker present).
  3. Optionally address the mixed kernel dmesg + mux frames on tty1 — either accept it or consider a dedicated UART if one is available.
- Updated: 2026-05-07

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
  without adding event IDs. The VM-fd poll slice is also done: kmod commit
  `ec0e7de vmfd: poll forwarded rpc readiness` makes the VM fd readable when the
  forwarded RPC queue has work, and QEMU commit `26be78e2a8 sel4: handle rpc
  through vmfd readiness` drains the same queue from the main loop, removing the
  dedicated seL4 virtio wait thread without changing the request ABI. Do not add
  old `SEL4_DELEG_*`, direct slots, mailbox, or generation fields.
- Orin AGX branch triage is now recorded separately. Keep real Orin platform,
  DTS, carefully sliced elfloader work, cross-VM IRQ, control-dataport support,
  the VM DTB dump hook, and the `sel4utils_elf_reserve()` NULL-vspace fix. Do
  not replay the old cacheability attempts, SDEI/RAS-hunting changes, or
  hyp-ftrace path. Try without forced `clean_cache=1` first, but retain it as a
  fallback experiment if symptoms justify it.
- Updated: 2026-05-04

### Cross-EL tracing implementation

- Status: paused
- Primary note: [../architecture/cross-el-tracing-feasibility.md](../architecture/cross-el-tracing-feasibility.md)
- Related notes:
  - [../plans/ftrace-upstream-integration-plan.md](../plans/ftrace-upstream-integration-plan.md)
- Last known state: tracing implementation has strict change control. Repos to be touched must be clean, work must happen on a dedicated branch, and commits should reference the design source.
- Next action: before any tracing code edits, verify repo cleanliness and request explicit approval for branch creation or dirty-repo handling.
- Updated: 2026-04-27

### Snapshot reader API, N-buffer design, topic projection, C++/Rust wrappers, Rust demo

- Status: **complete** (Linux path + Yocto build + on-hardware validation all done)
- Primary note: `projects/isengard-camkes-vm/docs/snapshot-reader-api.md`
- Related notes:
  - `sources/isengard-contracts/include/isengard/snapshot.h` — seqlock primitives; `static_assert` guard fixed for C++ (`!defined(__cplusplus)`) in commit `2121ff3`
  - `sources/isengard-contracts/include/isengard/snapshot_reader.h` — scheme-independent reader API; N-buffer scheme dispatch added (commit `b7dc472`)
  - `sources/isengard-contracts/include/isengard/snapshot_nbuffer.h` — 4-slot CAS-refcount pool (commits `b7dc472`, `afa302f`)
  - `sources/isengard-contracts/include/isengard/snapshot_topic.h` — topic descriptor types (commit `00493a0`)
  - `sources/isengard-contracts/include/isengard/snapshot.hpp` — C++11 header-only wrapper: `SnapshotPin` RAII, `topics::CanStatus/Runtime/Supply` (commit `2121ff3`)
  - `sources/isengard-app/platform/linux/isengard_objfs.c` — FUSE objfs with N-buffer pool, `/snapshots/` tree, `/topics/` projection layer (commits `a47590a`, `68f98a6`)
  - `sources/isengard-rs/` — Rust crate: `SnapshotPin<'pool>`, `SnapshotRegion` pread64 reader, `isengard_snapshot_watch` binary (commits `8a20327`, `1cd03e1`)
  - `sources/isengard-app/platform/linux/snapshot_memfd_demo.c` — added `--launch-watch-looping` mode (commit `8e494df`)
  - `meta-isengard/recipes-isengard/isengard-rs/isengard-rs_git.bb` — Yocto cargo+externalsrc recipe; `isengard-image-native.bb` includes `isengard-rs` (commit `81cdc13`)
- Last known state: Fully validated on real Orin AGX by Autopilot run `20260506-111643` (chain: `isengard-linux-orin-rust-demo`, overall_status: pass). Chain uploaded fresh `isengard-rootfs` (built `make isengard-linux-image` 2026-05-06), booted Isengard EFI, ran `isengard-demo-start` and `isengard_snapshot_watch`, and `verify_rust_output` passed. All steps ok.
- Next action: none — treat as closed. Next work is Phase 0/Phase 1 in `isengard-master-tracker.md` (CAN contract and first Orin Isengard app shape).
- Updated: 2026-05-06

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

- Status: paused
- Primary note: [../../../../../normet/docs/05-platform-next/trust-and-update/Rugged_x86_Secure_Boot_TPM_Attestation_And_BSP_Questions.md](../../../../../normet/docs/05-platform-next/trust-and-update/Rugged_x86_Secure_Boot_TPM_Attestation_And_BSP_Questions.md)
- Related notes:
  - [../../../../../normet/docs/05-platform-next/trust-and-update/Boot_Trust_And_Platform_Ownership_Comparison.md](../../../../../normet/docs/05-platform-next/trust-and-update/Boot_Trust_And_Platform_Ownership_Comparison.md)
  - [../../../../../normet/docs/05-platform-next/trust-and-update/Why_Update_And_Boot_Trust_Must_Be_First_Class_In_Normet_Platform_Design.md](../../../../../normet/docs/05-platform-next/trust-and-update/Why_Update_And_Boot_Trust_Must_Be_First_Class_In_Normet_Platform_Design.md)
- Last known state: EPEC rugged `x86` call-prep requirements were captured in a durable Normet note. The key questions are customer-owned `UEFI` Secure Boot `PK`/`KEK`/`db`/`dbx`, `TPM 2.0` type and provisioning, measured boot and attestation behavior, BSP source availability, arbitrary customer-signed EFI payloads, custom Linux or hypervisor/seL4 loader support, `VT-x`/`VT-d`/IOMMU exposure, update/recovery semantics, and the minimum engineering package to request from the vendor.
- Next action: deprioritised — Orin AGX hardware is the primary direction, making rugged x86 evaluation unlikely to be needed. Reopen only if a customer or project requirement forces a non-Orin x86 platform choice.
- Updated: 2026-05-04

### PX4 uORB access model

- Status: captured in a dedicated Normet platform-next target-architecture note.
- Primary note: `/home/hlyytine/normet/docs/05-platform-next/target-architecture/Normet_Object_Topic_Model_Inspired_By_PX4_uORB.md`
- Related notes:
  - PX4 uORB documentation: `https://docs.px4.io/main/en/middleware/uorb`
  - PX4 uORB manager source/API references for the `px4_open`/`px4_read` backing model.
- Last known state: PX4 uORB topics are exposed internally as file-like virtual device nodes, commonly visible under `/obj`, and the implementation uses file-descriptor style operations such as open/read/ioctl/poll. Normal application code should still be described as using the uORB pub/sub API (`orb_advertise`, `orb_publish`, `orb_subscribe`, `orb_copy`, or C++ wrappers), not as reading and writing ordinary persistent files. For Normet, the useful direction is a Normet-owned object/topic layer inspired by `uORB`: typed topics, generated contracts, topic inspection, logging/replay, and bridgeable state over coherent CANopen/IOmux snapshots, not direct adoption of `uORB` as the platform runtime.
- Next action: if prototyped, start with a small read-only CANopen/IOmux-derived topic projection above the coherent snapshot boundary and validate generation stability, freshness metadata, topic inspection, and logging before adding command/write paths.
- Updated: 2026-04-28

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
