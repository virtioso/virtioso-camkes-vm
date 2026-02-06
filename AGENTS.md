# AGENTS.md

This file provides guidance to Codex (and other coding agents) for working in this repository.

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

6. `/home/hlyytine/pkvm/autopilot/docs/ai-interactive-console.md`

Note: `AUTOPILOT_DIR` is the working directory (queues/results/profiles),
not the code path. Code lives in `/home/hlyytine/pkvm/autopilot`.

## MCP Server (Autopilot) Availability

This repo provides an MCP server definition at `~/tii-sel4/.mcp.json`:

- Server name: `sel4-autopilot`
- Command: `python3 /home/hlyytine/pkvm/autopilot/sel4_mcp_server.py`
- Default `AUTOPILOT_DIR`: `/home/hlyytine/tii-sel4/autopilot`

Some clients auto-load MCP servers from `.mcp.json`; some do not.
If MCP is unavailable, fall back to the request/result queues in `AUTOPILOT_DIR`.

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
