# Making Autopilot Usable For Agents

Status: draft
Updated: 2026-04-28

## Problem

Autopilot currently exposes more than one practical control surface to agents:

- MCP tools
- Python CLI / client scripts
- direct queue-file manipulation
- direct daemon/log/internal inspection

In practice this creates inconsistent behavior. Agents submit through one path,
debug through another path, and sometimes patch Autopilot internals when the API
surface is the real problem. The result is hard to reason about and creates
extra operational burden, especially because MCP interface changes require
client/session refreshes outside the agent's reliable control.

## Goals

1. Define exactly one supported agent-facing Autopilot API.
2. Forbid mixed API use in `AGENTS.md`, playbooks, and runbooks.
3. Make normal build/test validation possible without inspecting Autopilot
   internals.
4. Make API failure a stop condition: if the chosen API cannot express the
   needed operation or returns ambiguous/broken results, the agent must stop
   and ask the human owner instead of debugging or modifying Autopilot.
5. Provide machine-readable status plus short interpretation guidance for
   agents.
6. Make daemon startup deterministic by clearing stale pending and processing
   requests before accepting new work.

## Preferred Direction

Use a command-line API named `autopilot` in `PATH`.

The command should be a stable wrapper around the real Autopilot implementation,
not a second implementation. It should return JSON by default for agent use and
should avoid requiring MCP server reloads after Autopilot changes.

Candidate command shape:

```sh
autopilot status --json
autopilot start --platform orin-agx-uefi-netboot --tmux --json
autopilot restart --platform orin-agx-uefi-netboot --tmux --json
autopilot submit efi --chain vm-qemu-virtio --binary /abs/path/to/image --json
autopilot get <request-id> --json
autopilot logs <request-id> --json
```

The command should report enough structure for agents to decide:

- whether the daemon is running
- whether prepare is pass, probing, degraded, or failed
- whether queue entries are pending, processing, completed, or failed
- whether startup cleared stale pending/processing requests
- whether a submitted request was accepted, rejected, or blocked
- what the next safe action is

## Startup Queue Policy

On every Autopilot daemon startup, clear all requests from `pending` and
`processing` before the daemon accepts new work.

Rationale:

- if Autopilot restarts, the previous daemon either crashed, was stopped, or was
  modified to better suit the current work
- pending requests were submitted against the old runtime state and should not
  be treated as durable intent
- processing requests have lost their executor and should not be resumed as if
  the execution context were intact
- agents should not have to inspect queue directories or infer whether old work
  is safe to continue

The startup status JSON should explicitly report the cleanup action, for
example:

```json
{
  "startup_cleanup": {
    "pending_cleared": ["20260428-114335"],
    "processing_cleared": [],
    "policy": "clear-pending-and-processing-on-start"
  }
}
```

## Agent Rules

Once the command API is selected, agent instructions should say:

- Use only `autopilot ...` for Autopilot lifecycle, submit, status, and result
  retrieval.
- Do not use MCP tools for Autopilot unless the policy is explicitly changed
  back to MCP.
- Do not write request files directly.
- Do not call Python Autopilot internals directly.
- Do not inspect or patch Autopilot internals to work around API behavior.
- If `autopilot ... --json` fails, returns invalid JSON, lacks a needed
  operation, or gives an ambiguous state, stop and report the exact command,
  output, and requested operation to the human owner.

## CI Tester Skill Option

A dedicated `tester` or `ci-tester` skill could hold interpretation rules for
Autopilot JSON and build/test verdicts.

Pros:

- keeps long interpretation rules out of `AGENTS.md`
- gives agents a focused checklist for queue state, result status, and failure
  boundaries
- can distinguish build failure, submit failure, queue/prepare failure, and
  guest/runtime failure in a reusable way

Cons:

- skills are only useful if agents reliably load them for test work
- critical safety rules still need to live in `AGENTS.md` / runbooks, not only
  in a skill
- skill and command JSON schema can drift unless the command output links to
  the authoritative interpretation document

Recommended split:

- `AGENTS.md` and runbooks contain the hard policy: one API only, no internals,
  stop on API failure.
- `docs/agents/autopilot-testing-policy.md` contains the canonical command
  contract and state interpretation.
- an optional `ci-tester` skill can summarize and operationalize that policy for
  test-heavy sessions.

## Next Actions

1. Decide whether MCP is retired for agent use or kept only as an internal/non-
   default integration.
2. Define the minimal `autopilot` CLI JSON schema for `status`, `submit`, and
   `get`.
3. Define startup cleanup semantics: clear `pending` and `processing` on daemon
   start and report the cleared request IDs in JSON.
4. Update `docs/agents/autopilot-testing-policy.md` to make the chosen API the
   only allowed control surface.
5. Update `projects/virtioso-camkes-vm/AGENTS.md` and runbooks to forbid mixed
   MCP/CLI/queue usage.
6. Only after the policy is accepted, implement or wire the `autopilot` command.
