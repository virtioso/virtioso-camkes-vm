# Autopilot Testing Policy

This file is the canonical policy for Autopilot test submission and results.

## Required API

- Use `mcp__sel4-autopilot__test_sel4_efi` for EFI test submission.
- Use `mcp__sel4-autopilot__get_test_status` or `mcp__sel4-autopilot__wait_for_test` for status.
- Use `mcp__sel4-autopilot__get_logs` for logs.
- For daemon lifecycle operations, use `mcp__sel4-autopilot__autopilot_start` or
  `mcp__sel4-autopilot__autopilot_restart` with `use_tmux=true`.

## Required Parameters

- Always pass `autopilot_dir="/home/hlyytine/tii-sel4/autopilot"`.
- For daemon start/restart calls, always pass `use_tmux=true`.
- Use Autopilot chains, not legacy profiles.
- For QEMU defconfig targets, use `chain = target`.
- Do not verify legacy profile files before submit.

## Queue and Results

- A new test submit is blocked while another request is pending/processing.
- Console logs are stored under:
  `results/<id>/console/`
- Guest DTB/DTS exports are stored under:
  `results/<id>/device-trees/`

## Scope Rule

- MCP is for testing and result retrieval in this workflow.
- Build operations are executed with runbook `make` commands.
