# AGENTS.md

This file provides guidance to Codex (and other coding agents) for working in this repository.

Canonical file: `projects/virtioso-camkes-vm/AGENTS.md`.
Workspace root `AGENTS.md` is only a symlink/linkfile to the canonical file.
Edits made via either path affect the same file; treat `projects/virtioso-camkes-vm/AGENTS.md` as the source of truth.

## Knowledge Graph Tool (Orthanc)

For AI-assisted architecture and project management work using idea graphs,
see `~/orthanc/AGENTS.md`. The `/ideas-graph` skill provides shorthand access.

Orthanc is a weighted association graph system: nodes are architectural ideas,
edges are weighted associations, and every mutation is logged with reasoning.
The `isengard` graph (30 nodes, 92 edges) covers Isengard architecture.

---

## Current Investigation Focus

- Topic: Orin AGX CAmkES component mux/demux completion.
- Working notes (primary): `docs/architecture/cross-arch-tcu-uart-mux-plan-2026-05-01.md`.
- Current status: finish the mux/demux path on real Orin AGX first. Keep the
  shared `0xfe` TCU-style protocol, generated CAmkES stream identity, runtime
  announcements, demux-created channels, and Autopilot introspection as the
  target. Put x86 mux completion into backlog/regression unless a shared fix is
  needed for Orin.

## Defaults (Per User Request)

- Default target for build/test requests is **Orin AGX**.
- Always perform a **clean rebuild** unless the user explicitly asks for incremental.

## Canonical Sources (SSOT)

Operational policy and command sequences are authoritative only in:

1. `projects/virtioso-camkes-vm/AGENTS.md`
2. `docs/agents/task-router.md`
3. `docs/agents/build-test-runbook.md`
4. `docs/agents/autopilot-testing-policy.md`
5. `docs/agents/preflight-policy.md`
6. `docs/agents/repo-topology-policy.md`
7. `docs/agents/example-workflows-fastpath.md`

## Isengard Documentation Rule

All durable Isengard documentation — roadmaps, trackers, architecture notes,
design plans, and investigation logs — must live in `sources/isengard-core/docs/`.

Do not create or move Isengard documentation into `projects/isengard-camkes-vm/docs/`.
That directory is reserved for seL4-side deployment notes, CAmkES wiring docs, and
hardware-description references that are inherently bound to the
`projects/isengard-camkes-vm` repo.

The current authoritative Isengard documents are:
- `sources/isengard-core/docs/trackers/isengard-master-tracker.md` — active task and
  decision tracker
- `sources/isengard-core/docs/trackers/normet-program-roadmap.md` — management-level
  program roadmap and cross-team discovery guide
- `projects/isengard-camkes-vm/docs/normet-isengard-project-direction.md` —
  strategic direction (exception: lives in `isengard-camkes-vm` because it
  predates this rule and is closely tied to seL4-side placement decisions)

## IOmux Binding Types SSOT

The Normet ns3iomux JSON machine-configuration format (`~/normet/ns3iomux/machines/`)
is the authoritative definition for signal binding types (`DirectMidPin`,
`ExertusModule`, `RawCan`).  The Rust enum `CpSource` in `isengard-core` is an
implementation mirror of that schema.

When designing, implementing, or documenting these types, treat the JSON schema as
ground truth, not the Rust code or internal prose descriptions.

## Continuity / Lost Context Recovery

When context appears lost, or the user asks whether we were working on a topic,
first check `docs/agents/context-index.md`.

When creating or substantially updating a durable plan, investigation note, or
long-running work item, update the context index with the primary file, current
status, last known state, and next action.

## Fast Path

For “build and test `vm_qemu_virtio` on Orin AGX”:

1. `make mrproper`
2. `make orinagx_defconfig`
3. `make vm_qemu_virtio`
4. `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot submit efi --chain vm-qemu-virtio --binary /home/hlyytine/tii-sel4/orinagx_vm_qemu_virtio/images/capdl-loader-image-arm-orinagx --json`

For `qemu_x86_64_defconfig` QEMU-backed x86 validation:

1. `make mrproper`
2. `make qemu_x86_64_defconfig`
3. `make vm_qemu_virtio`
4. `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot submit efi --chain qemu_x86_64_defconfig --binary /home/hlyytine/tii-sel4/qemu_x86_64_vm_qemu_virtio/images/capdl-loader-image-x86_64-pc99 --build-platform qemu_x86_64 --json`

## Build/Test Policy

- Build instructions must be followed precisely, without adding or removing a single character.
- Build commands come from runbooks and are executed with `make <target>` from workspace root.
- Defconfig targets must also be executed from workspace root.
- Defconfig targets must be run in isolation. Do not execute `make <name>_defconfig`
  in parallel with any other command, including status checks, file existence
  checks, or build commands.
- Do not switch to repo-local build entrypoints such as `make -C virtioso-build ...`
  for canonical configuration/build flows, even if they appear to work around
  a local workspace issue. If the workspace-root path fails, treat that as a
  real issue to be fixed or reported, not as permission to change the entrypoint.
- For Yocto module-only rebuild of `kmod-sel4-virt`, use `make kmod-sel4-virt`.
  - Default is clean (`bitbake -c cleansstate kernel-module-sel4-virt` then build).
  - Use `YOCTO_INCREMENTAL=1 make kmod-sel4-virt` only when explicitly requested.
- Use only the `autopilot` command-line API for test submission/status/log
  retrieval, not MCP, direct queue files, or Python Autopilot internals.
- If `autopilot ... --json` fails, returns invalid JSON, lacks the needed
  operation, or gives an ambiguous state, stop and report the exact command,
  output, and requested operation to the human owner.
- Canonical EFI test command is `autopilot submit efi`.
- Use Autopilot chains, not legacy profiles.
- For QEMU defconfig targets, use the same underscore form for the Autopilot
  chain as for the build target.
- Always pass `--autopilot-dir /home/hlyytine/tii-sel4/autopilot` to
  `autopilot`.
- When reporting VM-specific console evidence from mux/demux Autopilot runs,
  always check the demuxed `vmX_guest_console_sink` log for the target VM
  first. For VM1 `uname -a` proof, prefer
  `results/<id>/console/console-runtime/vcmuxer_logs/vm1_guest_console_sink.txt`
  over raw host captures such as `tty1.raw`; raw captures may show firmware or
  carrier traffic rather than the logical VM stream.
- `qemu_x86_64_defconfig` is an autopilot-backed remote QEMU target; prefer the
  autopilot path for test execution and only fall back to direct
  `tools/qemu_runner.py` debugging when the autopilot backend itself is the
  thing under investigation.
- For Orin `vm_qemu_virtio`, cross-VM connector IRQ reserve must remain 8-bit safe (`<=255`)
  unless full-width DT IRQ mapping support is explicitly implemented.
- When VM passthrough IRQ inputs change (`vm*.dtb_irqs`, platform IRQ reserves, or PCI INTx usage),
  recompute the free 8-bit IRQ range and request explicit human affirmation before changing
  `free_plat_interrupts[]`.

## Preflight

- Use task-based preflight only: `docs/agents/preflight-policy.md`.
- Do not read broad investigation material for routine build/test tasks.

## Session Warm-Start Defaults

- For Normet / Isengard architecture or platform-direction work, open
  `projects/isengard-camkes-vm/docs/normet-isengard-project-direction.md`
  first, then `sources/isengard-core/docs/trackers/isengard-master-tracker.md`
  for active tasks, dependencies, open decisions, and validation expectations.
  Current strategic defaults: Orin-family rugged hardware is the primary
  direction; both plain Linux and seL4-with-Linux-VM placements remain open;
  target priority is Orin AGX, then `qemu_arm64`, then `qemu_x86_64`, then real
  x86; reusable code should move out of x86-only app directories into shared
  `sources/` or shared Virtioso/Isengard substrate before adding Arm support.
- For requests mentioning `projects/virtioso-camkes-vm/apps/Arm` or
  `projects/vm-examples/apps/Arm`, route immediately via
  `docs/agents/example-workflows-fastpath.md` instead of broad repo scanning.
- For requests on Yocto-built VM images (`vm-images/*`), scope context to
  touched recipe/source paths first; do not restudy whole
  `vm-images/virtioso-yocto-layers/` unless the request explicitly asks for a
  full-layer review.
- For requests like “continue Isengard stuff”, route immediately to
  `projects/isengard-camkes-vm/docs/normet-isengard-project-direction.md`,
  then open `sources/isengard-core/docs/trackers/isengard-master-tracker.md` for
  active tasks. Treat `projects/isengard-camkes-vm` as the seL4-side repo
  plus `sources/isengard-core` as the current source-side implementation repo.
  Linux-side software for that plan must go through a dedicated Yocto layer
  `vm-images/meta-isengard`.
- For `projects/virtioso-camkes-vm/apps/x86/vm_qemu_virtio`, treat the current
  app as leftover early-Isengard scaffolding, not the target architecture.
  The target is to replicate the Arm `vm_qemu_virtio` shape on `qemu_x86_64`:
  two VMs, VM0 booting `vm-image-driver`, VM1 booting `vm-image-user` from
  within VM0, with no Isengard-specific CAN/Kvaser/native PCI service logic in
  the x86 app itself. Do not optimize the x86 wait path before that target
  topology and the physical-PCI-plus-vPCI coexistence model are understood.

Note: `AUTOPILOT_DIR` is the working directory (queues/results/runtime),
not the code path. Autopilot chain definitions live in the Autopilot codebase
and are the source of truth. Code lives in `/home/hlyytine/autopilot`.

## Autopilot Command API

The only supported agent-facing Autopilot API in this workspace is the
`autopilot` command in `PATH`.

Required command pattern:

```bash
autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot <command> --json
```

Do not use MCP tools, direct request/result queue manipulation, or Python
Autopilot internals as fallback paths. If the command API cannot do the needed
operation or its JSON is invalid/ambiguous, stop and ask the human owner.

When starting or restarting Autopilot from this workspace, always set
`WORKSPACE=/home/hlyytine/tii-sel4` explicitly in the command environment.
Do not rely on inherited shell state for `WORKSPACE`.

Use `--tmux` for daemon start/restart. Startup clears all pending and processing
requests before accepting new work; old queue entries are not durable intent
after restart.

Orin AGX daemon start/restart:

```bash
WORKSPACE=/home/hlyytine/tii-sel4 autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot restart --platform orin-agx-uefi-netboot --tmux --tty0 /dev/ttyACM0 --tty1 /dev/ttyACM1 --json
```

## Yocto Local Source Policy

- Active Yocto layer is `vm-images/virtioso-yocto-layers/meta-virtioso-sel4/`.
- Edit Yocto-managed sources only in:
  - `sources/qemu`
  - `sources/kmod-sel4-virt`
  - `sources/sel4-linux-kernel-support`
- Before `bitbake`, these source repos must be clean (no staged/unstaged/untracked files).
- `sources/qemu` submodules must be initialized before bitbake.
- Use `make linux-image` as canonical full Yocto image build entrypoint.
- Use `make kmod-sel4-virt` for the module-only Yocto recipe path.

## Repo Roots & Symlinked Paths

This workspace is a repo manifest checkout, not a single git repository. Many
paths at `~/tii-sel4` are linkfiles/symlinks into other repos.

Before `git add` or `git commit`, always work from the real repo root:

```bash
git -C <path> rev-parse --show-toplevel
```

If you are editing `docs/` (linked to this repo), commit from:
`projects/virtioso-camkes-vm/`.

## Additional Notes

- When unsure, prefer `docs/agents/*` for operational guidance.
- Treat investigation and plan docs as technical context, not command authority.

## Tracing Implementation Change-Control Policy

Applies to cross-repo tracing implementation work.

1. Before any tracing implementation edits, all manifest repos to be touched
   must be in clean state (no staged/unstaged/untracked files).
2. If a repo is dirty, stop and request explicit human approval for exactly one
   action per repo: commit, stash, or discard.
3. Never implement tracing work on detached `HEAD` or upstream branches.
4. Create and use a dedicated implementation branch per target repo, default
   name: `virtioso-next-tracing`.
5. Branch creation is allowed only after repo cleanliness is verified and human
   approval is recorded.
6. Commit in atomic logical units; each commit message should reference the
   design source:
   `docs/architecture/cross-el-tracing-feasibility.md`.
7. Keep rollback simple:
   - capture pre-implementation `HEAD` per repo
   - optionally create baseline tag before first tracing commit
8. Request human approval before:
   - resolving dirty repos
   - creating tracing branches
   - discarding local changes
   - finalizing milestone-level integration/pin updates
