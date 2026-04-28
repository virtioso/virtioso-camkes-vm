# Making Autopilot Usable For Agents

Status: active implementation
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

## Concrete Autopilot Fix Plan

This backlog is based on the first Orin AGX `vm_qemu_virtio` run through the
new command API:

- request: `20260428-122927`
- result: `failed`
- failure marker: `AUTOPILOT_FAIL: ANALYSIS_HOOKS_FAILED (REQUIRED_HOOK_FAILED)`
- useful evidence: driver VM booted, `uservmctl start` ran, VM1/user VM exited,
  and the user VM log contained `ERROR: vio_trace not ready`

### Slice 1: Make Daemon And Queue State Non-Contradictory

Problem observed:

- `autopilot get 20260428-122927 --json` reported `status=processing`.
- `autopilot status --json` simultaneously reported `daemon.running=false` and
  `cmdline=["-bash"]`, while still listing the request as current.

Fix:

1. Split daemon status into explicit fields:
   - `manager_process`
   - `worker_process`
   - `tmux_session`
   - `current_request`
2. Stop deriving daemon liveness from the tmux shell process.
3. Report a distinct state when the tmux session exists but the worker process
   has exited while a request remains in `processing`.
4. Treat that combination as an API-level anomaly with an explicit
   `api_health.status="inconsistent"` field.

Acceptance criteria:

- During an active run, `autopilot status --json` reports the real worker PID
  and `daemon.running=true`.
- If the worker exits while a request is current, JSON says so directly and
  names the affected request.
- No successful command prints a Python traceback for state-reporting failures.

### Slice 2: Make Final Results Self-Contained

Problem observed:

- The terminal result only exposed `ANALYSIS_HOOKS_FAILED`.
- The agent had to request large console logs to infer that
  `ERROR: vio_trace not ready` was the relevant failure line.

Fix:

1. Extend each required analysis hook to emit structured failure data:
   - `hook_name`
   - `required`
   - `expected`
   - `observed`
   - `matched_log`
   - `evidence_excerpt`
   - `source_file`
2. Add this to `autopilot get <id> --json` under `failure`.
3. Keep `chain_summary` as the high-level verdict, but make `failure` the
   agent-facing explanation.

Acceptance criteria:

- For a failed request, `autopilot get <id> --json` is enough to answer
  "what failed and why" without a separate log read.
- The `20260428-122927` failure class would be represented as a named hook
  failure with `ERROR: vio_trace not ready` as evidence.

### Slice 3: Add Focused Evidence Commands

Problem observed:

- `autopilot logs <id> --include-contents --json` emitted very large log
  payloads that are easy for clients to truncate.

Fix:

1. Add bounded log retrieval options:
   - `autopilot logs <id> --tail 200 --json`
   - `autopilot logs <id> --file tty0.filtered.log --tail 200 --json`
   - `autopilot logs <id> --grep AUTOPILOT_FAIL --json`
2. Add a summary command:
   - `autopilot evidence <id> --json`
3. Make `evidence` return:
   - fail markers
   - required-hook failure details
   - last successful chain step
   - key guest/runtime markers
   - log file paths and byte offsets for the excerpts
4. Include truncation metadata whenever content is shortened.

Acceptance criteria:

- No normal failure investigation requires dumping full raw console logs.
- `autopilot evidence 20260428-122927 --json` would return the fail marker,
  `uservmctl status=exited`, and `ERROR: vio_trace not ready`.

### Slice 4: Make `get` And `status` Agree On Result Paths

Problem observed:

- While the request was processing, `autopilot status --json` exposed
  `result_dir`, but `autopilot get <id> --json` returned `result_dir=null`.

Fix:

1. Use a single request-state model for `get` and `status`.
2. Populate `result_dir` as soon as the processing result directory exists.
3. Add `artifacts.console_dir` and `artifacts.available_logs` when known.

Acceptance criteria:

- `get` and `status` report the same `result_dir` for the same request state.
- Agents can discover available logs from `get` without calling `logs` first.

### Slice 5: Make CLI Help And Errors Robust

Problem observed:

- `autopilot logs --help` failed before printing help because `WORKSPACE` was
  not set.
- `restart --platform orin-agx-uefi-netboot --tmux` without UARTs produced a
  Python traceback instead of JSON.

Fix:

1. Delay runtime configuration loading until after argument parsing and help.
2. Convert expected validation failures into JSON errors:
   - `status="error"`
   - `error.kind="validation"`
   - `error.message`
   - `error.required_args`
3. Add platform defaults or explicit config lookup for known platforms where
   that is safe.

Acceptance criteria:

- `autopilot --help` and `autopilot <subcommand> --help` work without
  `WORKSPACE`.
- Missing required UARTs produce valid JSON, not a traceback.
- If defaults are available for `orin-agx-uefi-netboot`, restart can use them
  without repeating `--tty0 /dev/ttyACM0 --tty1 /dev/ttyACM1`.

### Slice 6: Clarify Required Hook Policy

Problem observed:

- The run failed because a required analysis hook failed, but the API did not
  clearly state whether `vio_trace` is mandatory test success evidence or only
  diagnostic instrumentation.

Fix:

1. Define hook severity per chain:
   - `required_for_pass`
   - `diagnostic_only`
   - `warning`
2. For `vm-qemu-virtio`, decide whether `vio_trace` readiness is a required
   success criterion.
3. Report hook policy in `autopilot get <id> --json` and `autopilot evidence`.

Acceptance criteria:

- A `vio_trace not ready` observation either fails the test with a named
  required hook, or appears as a warning while another required condition
  decides the verdict.
- The JSON makes that policy visible without reading Autopilot code.

## Implementation Order

Recommended order:

1. Fix CLI robustness first: help without `WORKSPACE`, JSON validation errors,
   and restart UART defaults/errors.
2. Unify request-state modeling so `get` and `status` cannot disagree on
   daemon/current/result-directory state.
3. Add structured hook failure data to result generation and expose it from
   `get`.
4. Add `evidence` and bounded `logs` options.
5. Decide and encode `vio_trace` hook severity for `vm-qemu-virtio`.
6. Re-run Orin AGX `vm_qemu_virtio` and require that the final answer can be
   produced from `autopilot get` plus, at most, `autopilot evidence`.

Commit strategy:

- one commit per slice in `/home/hlyytine/autopilot`
- one follow-up docs commit in this repo after the CLI/result schema changes
  are real
- do not modify active run queues by hand during implementation

## Next Actions

Completed implementation slices:

1. Autopilot daemon startup now clears `pending` and `processing` requests and
   writes startup-cleanup JSON.
2. `/home/hlyytine/autopilot/bin/autopilot` provides the agent command API.
3. `/home/hlyytine/.local/bin/autopilot` points to the command wrapper.
4. Active workspace policy docs now require `autopilot ... --json` and forbid
   MCP/direct queue/Python-internals fallback paths.

Remaining work:

1. Decide whether to create an optional `ci-tester` skill for result
   interpretation.
2. Exercise the full Orin AGX and QEMU submit/status/get/logs path through the
   new command API.
3. Remove or clearly mark any remaining legacy MCP guidance outside active
   runbooks as historical.
