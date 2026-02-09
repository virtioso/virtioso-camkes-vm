# AGENTS.md

This file provides guidance to Codex (and other coding agents) for working in this repository.

Canonical file: `projects/virtioso-camkes-vm/AGENTS.md`.
Workspace root `AGENTS.md` is only a symlink/linkfile to the canonical file.
Edits made via either path affect the same file; treat `projects/virtioso-camkes-vm/AGENTS.md` as the source of truth.

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

## Fast Path

For “build and test `vm_qemu_virtio` on Orin AGX”:

1. `make mrproper`
2. `make orinagx_defconfig`
3. `make vm_qemu_virtio`
4. `mcp__sel4-autopilot__test_sel4_efi(..., profile="vm-qemu-virtio")`

## Build/Test Policy

- Build instructions must be followed precisely, without adding or removing a single character.
- Build commands come from runbooks and are executed with `make <target>` from workspace root.
- For Yocto module-only rebuild of `kmod-sel4-virt`, use `make kmod-sel4-virt`.
  - Default is clean (`bitbake -c cleansstate kernel-module-sel4-virt` then build).
  - Use `YOCTO_INCREMENTAL=1 make kmod-sel4-virt` only when explicitly requested.
- Use MCP for test submission/status/log retrieval, not for build commands in this workflow.
- Canonical EFI test tool is `test_sel4_efi`.
- Profile mapping is `profile = target.replace("_", "-")`.
- Verify `/home/hlyytine/autopilot/profiles/<profile>.json` before submitting tests.
- Always pass `autopilot_dir="/home/hlyytine/tii-sel4/autopilot"` to MCP tools.
- For Orin `vm_qemu_virtio`, cross-VM connector IRQ reserve must remain 8-bit safe (`<=255`)
  unless full-width DT IRQ mapping support is explicitly implemented.
- When VM passthrough IRQ inputs change (`vm*.dtb_irqs`, platform IRQ reserves, or PCI INTx usage),
  recompute the free 8-bit IRQ range and request explicit human affirmation before changing
  `free_plat_interrupts[]`.

## Preflight

- Use task-based preflight only: `docs/agents/preflight-policy.md`.
- Do not read broad investigation material for routine build/test tasks.

Note: `AUTOPILOT_DIR` is the working directory (queues/results/runtime),
not the code path. Profiles are static data and live in
`/home/hlyytine/autopilot/profiles` (single source of truth). Code lives in
`/home/hlyytine/autopilot`.

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
