# X86 `vm_qemu_virtio` Console Stream Inventory

Date: 2026-04-24

Tracker: [Console Router And Timeline Implementation Plan](console-router-and-timeline-implementation-plan-2026-04-23.md)

Current architecture SSOT:

- [../architecture/console-transport-and-routing.md](../architecture/console-transport-and-routing.md)
- [../architecture/autopilot-console-source-integration.md](../architecture/autopilot-console-source-integration.md)

This note remains a dated x86 stream inventory for the April 2026 investigation,
not the primary architecture document.

## Summary

For the current x86 `qemu_x86_64` `vm_qemu_virtio` investigation, the stream
names are no longer interchangeable. `driver_vm_console`,
`vmm_mux_control`, and `vmm_debug` are different architectural seams with
different producers, different consumers, and different testing meaning.

The practical rule is:

- use `driver_vm_console` for VM0 login and shell automation
- use `tty0` only as launch-compatibility and early runner-failure coverage
- use `vmm_mux_control` for legacy VMM mux annotations such as `vm0:` / `vm1:`
- use `vmm_debug` only for explicit dedicated debug prefixes such as
  `[vmmdbg] ` / `vmmdbg: `

## Current State

### Execution Path

The current x86 console path has three ownership layers:

1. `qemu_runner.py` declares the logical channels and emits
   `console-manifest.json`.
2. `console_router.py` executes the selected transport, creates
   `console-runtime/runtime-manifest.json`, per-channel logs, PTYs for
   interactive channels, and `console-runtime/sessions.json`.
3. Autopilot `chain_runtime.py` resolves logical sources from the manifest plus
   sessions metadata and then lets chains read from, or write to, those named
   sources.

Code references:

- runner channel and manifest definition:
  tools/qemu_runner.py
- router transport and runtime/session generation:
  tools/console_router.py
- Autopilot router-session discovery and PTY write resolution:
  /home/hlyytine/autopilot/chain_runtime.py

### Transport Modes

There are three distinct transport shapes in the current code:

1. `process_stdio`
   - one merged launcher-owned channel, `merged_console`
   - backward-compatible topology
   - no source-specific split beyond the merged stream
2. `line_prefixes`
   - current x86 compatibility demux path
   - classifies lines by prefix and routes unmatched text to a fallback channel
   - this is the active bridge for x86 login work
3. `jsonl_frames`
   - future source-tagged producer path
   - can represent the full logical channel set without prefix guessing

Only `line_prefixes` currently distinguishes `driver_vm_console`,
`vmm_mux_control`, and `vmm_debug` in the x86 remote-QEMU lane.

## Stream Inventory

### `tty0`

- Role: launch-compatibility source bound by `map_command_source`
- Producer: runner stdout/stderr compatibility path
- Consumer use: early `QEMU_RUNNER_ERROR:` and process-exit detection; legacy
  wait patterns
- Important constraint: once split routing is enabled, `tty0` is not the
  source of truth for VM0 login readiness

### `driver_vm_console`

- Role: VM0 guest console and default interactive input channel
- Producer:
  - `line_prefixes` fallback path for unprefixed guest-visible text
  - future `jsonl_frames` producer-tagged VM0 console frames
- Consumer use:
  - `send_cmd` login injection
  - prompt/shell matching
  - `uservmctl` command execution from the driver VM shell
- Current x86 meaning: this is the stream that should decide whether VM0
  reached `driver-vm login:` or a root shell

### `driver_vm_control`

- Role: reserved interactive control lane for VM0-side control traffic
- Producer: declared in the logical channel set, but not populated by the
  current `line_prefixes` x86 compatibility path
- Consumer use: none in the current x86 chains
- Status: architectural seam exists; current producer path does not yet use it

### `vmm_mux_control`

- Role: legacy VMM serial-mux control/noise lane
- Producer: `line_prefixes` routing for:
  - `[vm0] `
  - `[vm1] `
  - `vm0: `
  - `vm1: `
- Consumer use:
  - diagnostics
  - proving that prefixed mux traffic is not the same thing as VM0 guest
    console traffic
- Important distinction: this is **not** the dedicated `vmm_debug` stream

### `vmm_debug`

- Role: dedicated VMM debug/heartbeat/progress lane
- Producer: `line_prefixes` routing for:
  - `[vmmdbg] `
  - `vmmdbg: `
- Consumer use:
  - investigation-specific VMM debug correlation
  - keeping explicit VMM diagnostics out of the VM0 prompt stream
- Important distinction: this is **not** the same as `vmm_mux_control`; the
  former is explicit debug tagging, while the latter is the legacy mux/control
  classification seam

### `nested_qemu_control`

- Role: reserved nested-QEMU lifecycle/control channel
- Producer: declared in the logical channel set, but not populated by the
  current x86 `line_prefixes` path
- Consumer use: intended future home for nested-QEMU launch/control output
- Status: logical channel exists before producer-side framing is complete

### `user_vm_console`

- Role: intended VM1 guest console
- Producer:
  - future `jsonl_frames` producer path
  - not reliably derived by the current x86 compatibility stream
- Consumer use: target source for user-VM prompt/readiness once producer-side
  separation is real
- Important constraint: the current x86 lane should not pretend this source is
  authoritative until the producer side emits it explicitly

### `trace_control`

- Role: reserved tracing/control stream
- Producer: declared in the logical channel set, not used by the current x86
  `line_prefixes` compatibility path
- Consumer use: future trace-side integration

## Current Producer Matrix

| Transport | Populated streams in current x86 practice | Notes |
| --- | --- | --- |
| `process_stdio` | `merged_console` | Legacy fallback only |
| `line_prefixes` | `driver_vm_console`, `vmm_mux_control`, `vmm_debug` | Current x86 login investigation path |
| `jsonl_frames` | full logical set is representable | Intended end-state, not current producer reality |

## Current Consumer Matrix

### Smoke And Login Chains

- qemu_x86_64_defconfig.json
  should use:
  - `driver_vm_console` for login/shell success
  - `tty0` for early runner failure detection
- qemu_x86_64_vm_qemu_virtio_minimal_login.json
  uses `driver_vm_console` as the interactive source for login injection
- qemu_x86_64_vm_qemu_virtio_uservm.json
  uses `driver_vm_console` for login and `uservmctl` orchestration

### Diagnostic Interpretation

- `vmm_mux_control` answers "what did the legacy VMM-prefixed mux stream say?"
- `vmm_debug` answers "what did explicitly tagged VMM debug emit?"
- neither one should be treated as proof that VM0 reached a usable shell

## Recommendations

### 1. Freeze stream semantics in docs and chains

Rationale:
The current problem is not missing names; it is drift between names, producer
rules, and chain expectations.

Affected areas:

- this document
- [console-router-and-timeline-implementation-plan-2026-04-23.md](console-router-and-timeline-implementation-plan-2026-04-23.md)
- Autopilot chain source selections

Short-term benefit:

- avoids treating `vmm_mux_control` and `vmm_debug` as aliases
- makes run interpretation more reliable

Migration implication:

- no transport rewrite required; only source-selection discipline

### 2. Treat "verdict sources" and "diagnostic sources" as separate chain concerns

Rationale:
The chain layer should encode which sources are allowed to determine pass/fail
versus which are only supporting evidence.

Affected areas:

- x86 QEMU-backed Autopilot chains

Short-term benefit:

- `driver_vm_console` can own login readiness
- `tty0`, `vmm_mux_control`, and `vmm_debug` can remain available without
  corrupting verdict logic

Migration implication:

- easy to validate one chain at a time

### 3. Keep `line_prefixes` as a compatibility bridge, not as the target model

Rationale:
The current prefix classification is useful, but it is still a recovery step
after the byte stream has already been merged.

Affected areas:

- `tools/qemu_runner.py`
- `tools/console_router.py`

Short-term benefit:

- preserves current x86 testing velocity

Migration implication:

- future producer work should move real ownership to `jsonl_frames` or another
  source-tagged producer path, while keeping the current compatibility mode
  available during migration

## Validation Path

Use the current x86 validation lanes:

1. clean rebuild:
   - `make mrproper`
   - `make qemu_x86_64_defconfig`
   - `make vm_qemu_virtio`
2. smoke/login validation:
   - `qemu_x86_64_defconfig`
   - `qemu_x86_64_vm_qemu_virtio_minimal_login`
   - `qemu_x86_64_vm_qemu_virtio_uservm`
3. inspect result artifacts:
   - `console/console-manifest.json`
   - `console/console-runtime/runtime-manifest.json`
   - `console/console-runtime/sessions.json`
   - per-channel `raw.log` and `events.jsonl`

Evidence that the seam is real:

- `driver_vm_console` carries `driver-vm login:` or a root shell
- `vmm_mux_control` carries legacy `vm0:` / `vm1:` prefixed traffic
- `vmm_debug` carries only explicit `vmmdbg`-prefixed traffic
- `send_cmd` to `driver_vm_console` creates `tx` events on that channel and is
  reflected in guest-visible console behavior

## Risks And Open Questions

- Verified: `vmm_debug` exists in the current logical channel set and has its
  own prefix classification rules.
- Verified: `vmm_mux_control` still exists and is intentionally separate.
- Verified: the current x86 compatibility path does not yet populate every
  declared logical channel.
- Strong inference: some older tracker wording still reflects the pre-`vmm_debug`
  naming era and should not be read as the current channel taxonomy.
- Open question: when producer-side framing lands, should `vmm_mux_control`
  remain as a legacy-only compatibility lane or be retired after migration?
