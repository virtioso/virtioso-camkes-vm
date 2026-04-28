# Autopilot Testing Policy

This file is the canonical policy for Autopilot test submission and results.

## Required API

- Use only the `autopilot` command-line API for Autopilot lifecycle, submit,
  status, and result retrieval.
- The command must produce JSON. Use `--json` in documented commands even though
  JSON is the default agent output.
- Do not use Autopilot MCP tools for this workflow.
- Do not write request files directly.
- Do not call Python Autopilot scripts or library internals directly.
- Do not inspect or patch Autopilot internals to work around API behavior.
- If `autopilot ... --json` fails, returns invalid JSON, lacks the needed
  operation, or gives an ambiguous state, stop and report the exact command,
  output, and requested operation to the human owner.

## Required Parameters

- Always pass `--autopilot-dir /home/hlyytine/tii-sel4/autopilot`.
- For daemon start/restart calls, always use tmux.
- Use Autopilot chains, not legacy profiles.
- For QEMU defconfig targets, use `--chain <target>`.
- Do not verify legacy profile files before submit.

## Queue and Results

- A new test submit is blocked while another request is pending/processing.
- On daemon startup, Autopilot clears all `pending` and `processing` requests
  before accepting new work. Treat cleared requests as abandoned old runtime
  intent, not as resumable work.
- `autopilot status --json` reports queue state and startup cleanup.
- Console logs are stored under:
  `results/<id>/console/`
- Guest DTB/DTS exports are stored under:
  `results/<id>/device-trees/`

## Scope Rule

- `autopilot` is the only supported test API for agents in this workflow.
- Build operations are executed with runbook `make` commands.
