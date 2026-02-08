# AGENTS.md

This file provides guidance to Codex (and other coding agents) for working in this repository.

Canonical file: `projects/virtioso-camkes-vm/AGENTS.md`.
Workspace root `AGENTS.md` is only a symlink/linkfile to the canonical file.
Edits made via either path affect the same file; treat `projects/virtioso-camkes-vm/AGENTS.md` as the source of truth.

## Defaults (Per User Request)

- Default target for build/test requests is **Orin AGX**.
- Always perform a **clean rebuild** unless the user explicitly asks for incremental.

## Build/Test Workflow (Canonical)

### Build Target Resolution

- Read the top-level `Makefile` and include `projects/*/Makefile.virtioso_build`.
- Valid VM targets are `vm_*` plus `vm_minimal`/`vm_multi` from `TARGETS`.
- If the request matches a Makefile target:
  - Build with `make <target>` from the workspace root.
  - For `sel4test`, use `make sel4test` (not MCP build).

### Test Submission (EFI)

- Use the MCP tool `test_sel4_efi` for all EFI tests.
- Determine profile name:
  - `target` with `_` → `-` (e.g., `vm_qemu_virtio` → `vm-qemu-virtio`)
  - `sel4test` uses profile `sel4test`
- Verify `/home/hlyytine/autopilot/profiles/<profile>.json` exists.
- If the profile does **not** exist, stop and design a test chain with the human.
- `/boot/efi` cleanup must be done in the profile chain (via `ssh_cmd`).
- Logs are only those defined by the profile and live under `results/<id>/console/`.
- After every test run, Autopilot exports guest DTB dumps to `results/<id>/device-trees/`
  as both `.dtb` and `.dts` (when DTB markers are present in logs).
- If guest behavior is unexpected, always inspect generated DTS files before further debugging.
- Requests do **not** use a `type` field.

### Yocto Local Source Policy

- Active Yocto layer is `vm-images/virtioso-yocto-layers/meta-virtioso-sel4/`. Treat `vm-images/meta-sel4/` as deprecated for current builds.
- For these components, always edit source in repo-managed trees under `sources/`:
  - `sources/qemu`
  - `sources/kmod-sel4-virt`
  - `sources/sel4-linux-kernel-support`
- Before running any `bitbake` command, required source repos must be clean:
  - no staged changes
  - no unstaged changes
  - no untracked files
- If a repo is dirty, stop and get human feedback. Then either commit or discard changes before continuing.
- QEMU submodules in `sources/qemu` must be initialized before bitbake.

### Yocto Build Output (Mandatory)

- Keep using `make linux-image` as the canonical entrypoint for Yocto image builds.
- In non-interactive agent/CI terminals, BitBake UI output may be sparse or delayed.
- Track progress by polling logs while `make linux-image` runs:
  - `vm-images/build/bitbake-cookerdaemon.log`
  - latest `vm-images/build/tmp/log/cooker/<machine>/*.log`

## Mandatory Preflight (Before Any Planning or Implementation)

Always read the following documents before you start planning or making changes:

1. `CLAUDE.md`
2. `docs/README.md`
3. `docs/index.md`

If the task involves Orin AGX or platform-specific debugging, also read:

4. `docs/platforms/orin-agx/orin-agx-debugging-guide.md`

Always use MCP tools for Orin AGX testing.

If the task involves kernel tracing, ftrace, or scheduler/IRQ behavior, also read:

5. `../../kernel/docs/ftrace.md`

If the task involves Autopilot or interactive console sessions, also read:

6. `/home/hlyytine/autopilot/docs/ai-interactive-console.md`

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

These run Autopilot headless in a tmux session and return an attach hint
(`tmux attach -t autopilot`) for TUI access.

Submitting a test via MCP auto-starts Autopilot if it is not running.

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

If the task involves build, Yocto, or CI/CD, also read:

7. `docs/build-system/build-architecture.md`
8. `docs/build-system/yocto-integration.md`
9. `docs/build-system/ci-cd.md`

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

- Treat these preflight docs as required context. If they are missing or outdated, note that explicitly before proceeding.
- When unsure, prefer the docs under `docs/` as the source of truth for this repository.
