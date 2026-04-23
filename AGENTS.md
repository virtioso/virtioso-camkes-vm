# AGENTS.md

This file provides guidance to Codex (and other coding agents) for working in this repository.

Canonical file: `projects/virtioso-camkes-vm/AGENTS.md`.
Workspace root `AGENTS.md` is only a symlink/linkfile to the canonical file.
Edits made via either path affect the same file; treat `projects/virtioso-camkes-vm/AGENTS.md` as the source of truth.

## Current Investigation Focus

- Topic: Orin AGX `vm_qemu_virtio` VM1 boot stall around `virtio_console_init`.
- Working notes (primary): `docs/integration/orinagx-vm-qemu-virtio-crossvm-irq-analysis-2026-02-09.md`.
- Current status: cross-VM IRQ path is active (`irq=236` injects successfully); investigation focus is now virtio-console init/probe behavior.

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

## Fast Path

For “build and test `vm_qemu_virtio` on Orin AGX”:

1. `make mrproper`
2. `make orinagx_defconfig`
3. `make vm_qemu_virtio`
4. `mcp__sel4-autopilot__test_sel4_efi(..., chain="vm-qemu-virtio")`

For `qemu_x86_64_defconfig` QEMU-backed x86 validation:

1. `make mrproper`
2. `make qemu_x86_64_defconfig`
3. `make vm_qemu_virtio`
4. `mcp__sel4-autopilot__test_sel4_efi(..., chain="qemu_x86_64_defconfig")`

## Build/Test Policy

- Build instructions must be followed precisely, without adding or removing a single character.
- Build commands come from runbooks and are executed with `make <target>` from workspace root.
- Defconfig targets must also be executed from workspace root.
- Do not switch to repo-local build entrypoints such as `make -C virtioso-build ...`
  for canonical configuration/build flows, even if they appear to work around
  a local workspace issue. If the workspace-root path fails, treat that as a
  real issue to be fixed or reported, not as permission to change the entrypoint.
- For Yocto module-only rebuild of `kmod-sel4-virt`, use `make kmod-sel4-virt`.
  - Default is clean (`bitbake -c cleansstate kernel-module-sel4-virt` then build).
  - Use `YOCTO_INCREMENTAL=1 make kmod-sel4-virt` only when explicitly requested.
- Use MCP for test submission/status/log retrieval, not for build commands in this workflow.
- Canonical EFI test tool is `test_sel4_efi`.
- Use Autopilot chains, not legacy profiles.
- For QEMU defconfig targets, use the same underscore form for the Autopilot
  chain as for the build target.
- Always pass `autopilot_dir="/home/hlyytine/tii-sel4/autopilot"` to MCP tools.
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

- For requests mentioning `projects/virtioso-camkes-vm/apps/Arm` or
  `projects/vm-examples/apps/Arm`, route immediately via
  `docs/agents/example-workflows-fastpath.md` instead of broad repo scanning.
- For requests on Yocto-built VM images (`vm-images/*`), scope context to
  touched recipe/source paths first; do not restudy whole
  `vm-images/virtioso-yocto-layers/` unless the request explicitly asks for a
  full-layer review.
- For requests like “continue Isengard stuff”, route immediately to
  `projects/isengard-camkes-vm/docs/linux-only-qemu-migration-plan.md` and
  treat `projects/isengard-camkes-vm` as the seL4-side repo plus
  `sources/isengard-core` as the current source-side implementation repo.
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

## MCP Server (Autopilot) Availability

This repo provides an MCP server definition at `~/tii-sel4/.mcp.json`:

- Server name: `sel4-autopilot`
- Command: `python3 /home/hlyytine/autopilot/sel4_mcp_server.py`
- Default `AUTOPILOT_DIR`: `/home/hlyytine/tii-sel4/autopilot`

Some clients auto-load MCP servers from `.mcp.json`; some do not.
If MCP is unavailable, fall back to the request/result queues in `AUTOPILOT_DIR`.

## Autopilot Daemon Control (MCP)

You can start/stop/restart Autopilot via MCP tools:
- `autopilot_start`
- `autopilot_stop`
- `autopilot_restart`
- `autopilot_status`

When starting or restarting Autopilot, always pass `use_tmux=true`:
- `mcp__sel4-autopilot__autopilot_start(..., use_tmux=true)`
- `mcp__sel4-autopilot__autopilot_restart(..., use_tmux=true)`

This guarantees an attachable tmux session and an attach hint
(`tmux attach -t autopilot`) for TUI access.

Submitting a test via MCP auto-starts Autopilot if it is not running.
Auto-start may not provide an attachable tmux session, so explicitly start or
restart with `use_tmux=true` when TUI attachability is required.

Orin AGX note: MCP auto-start uses default UARTs `/dev/ttyACM0` and
`/dev/ttyACM1`. Replace these for other platforms (e.g. Raspberry Pi 4 uses
`/dev/ttyUSB*`).

### Codex CLI MCP Setup (Manual)

If you are using the Codex CLI and MCP servers are not auto-loaded, add the
server explicitly:

```bash
codex mcp add sel4-autopilot -- python3 /home/hlyytine/autopilot/sel4_mcp_server.py
codex mcp list
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
