# Console Router And Timeline Implementation Plan

Date: 2026-04-23

## Summary

The current console path merges too much too early. In the QEMU-backed path,
outer QEMU output is still commonly collapsed onto `stdio`, and the higher
layers then try to recover source meaning from already-merged streams. That is
the wrong ownership model for reliable automation, guest attribution, and
timeline reconstruction.

The target architecture is:

- one authoritative inline router per logical console source
- raw PTY forwarding for human and tool interaction
- structured event capture as a side effect of the same byte path
- no separate competing "interactive transport" and "observation transport"

This plan is coexistence-first. It preserves the current workflows while
introducing a better console boundary under the existing QEMU runner and
Autopilot abstractions.

## Progress

- 2026-04-24: hardened `tools/qemu_runner.py` remote-QEMU execution so `ssh`/`scp`
  run non-interactively (`stdin=DEVNULL`) and default to `BatchMode=yes` plus
  `ConnectTimeout=10`. This closes a real failure mode in Autopilot-managed
  runs where runner subprocesses could silently block on stdin before the
  console router even started, which in turn made the split-source chain look
  broken when the actual fault was remote transport interactivity.
- 2026-04-24: tightened launch-failure surfacing for the managed x86 chain.
  `qemu_runner.py` now emits `QEMU_RUNNER_ERROR:` for subprocess failures, and
  the dedicated `qemu_x86_64_vm_qemu_virtio_uservm` chain treats that on legacy
  `tty0` as an early launch failure before split console sources exist.

- 2026-04-23: Plan created and refined with:
  - manifest-driven demux configuration
  - dedicated CAmkES-side mux component direction for single-uart targets
  - `ZF_LOG` routing via generic mux log channels
- 2026-04-23: Primary motivating use case clarified
  - `vm_qemu_virtio` validation must stop treating one evolving tty stream as
    if it were one stable speaker
  - the concrete problem is VM0 boot/login, helper control traffic,
    nested-QEMU launch output, and user-VM console output sharing one observed
    stream and forcing brittle regex matching
  - the router/demux work is therefore justified by a real VM workflow, not
    only by abstract timeline reconstruction
- 2026-04-23: Implementation started with launcher-side work in
  `projects/virtioso-camkes-vm/tools/qemu_runner.py`
  - target slice: generate a run manifest owned by the launcher/backend
  - rationale: this is the first reusable SSOT seam and does not require
    immediate Autopilot changes
- 2026-04-23: First implementation slice completed
  - `qemu_runner.py` now emits launcher-owned `console-manifest.json`
  - `bundle.json` now references that manifest as bundle metadata
  - current topology is represented honestly as one merged channel with
    `legacy_aliases: ["tty0"]`
  - verified with:
    - `python3 -m py_compile projects/virtioso-camkes-vm/tools/qemu_runner.py`
    - `python3 projects/virtioso-camkes-vm/tools/qemu_runner.py prepare-remote-bundle ...`
- 2026-04-23: Second implementation slice completed
  - added reusable `tools/console_router.py`
  - current scope:
    - validate `console-manifest.json`
    - normalize manifest data
    - prepare channel runtime layout under a chosen runtime directory
    - emit `runtime-manifest.json` plus per-channel metadata/log file stubs
  - rationale: makes the manifest executable as runtime state without tying the
    design to Autopilot
  - verified with:
    - `python3 -m py_compile projects/virtioso-camkes-vm/tools/console_router.py`
    - `python3 projects/virtioso-camkes-vm/tools/console_router.py describe-manifest ...`
    - `python3 projects/virtioso-camkes-vm/tools/console_router.py prepare-runtime ...`
- 2026-04-23: Third implementation slice completed
  - `console_router.py` now supports a live inline wrapper mode for the current
    `process_stdio` transport
  - current scope:
    - spawn a wrapped command
    - mirror command output to router stdout for compatibility
    - write per-channel raw logs
    - write per-channel event logs with monotonic/realtime ns timestamps
    - create PTY symlinks for interactive channels and forward output to them
    - forward PTY/stdin input back to the wrapped command
  - `qemu_runner.py` now:
    - marks the current merged console channel as PTY-capable
    - copies `console_router.py` into remote bundles
    - wraps `simulate` with `console_router.py run-command` in `run-bundle.sh`
  - verified with:
    - `python3 -m py_compile projects/virtioso-camkes-vm/tools/console_router.py projects/virtioso-camkes-vm/tools/qemu_runner.py`
    - standalone router smoke test via `run-command -- bash -lc 'printf ...'`
    - generated bundle contains `runtime/console_router.py`
    - generated bundle `run-bundle.sh` invokes the router
    - runtime directory contains `raw.log`, `events.jsonl`, and `pty` symlink
- 2026-04-23: Fourth implementation slice completed
  - local QEMU runner path now goes through the same router seam as the remote
    bundle path
  - current scope:
    - `run_local()` writes a temporary launcher-owned manifest
    - `run_local()` invokes `console_router.py run-command` around the wrapped
      local command
  - rationale: keeps console architecture consistent across local and remote
    QEMU-backed flows
  - verified with:
    - `python3 -m py_compile projects/virtioso-camkes-vm/tools/qemu_runner.py`
    - `python3 projects/virtioso-camkes-vm/tools/qemu_runner.py run-local ... --dry-run`
- 2026-04-23: Fifth implementation slice completed
  - live router now emits `sessions.json` in the runtime directory
  - current scope:
    - stable session discovery metadata for consumers
    - includes logical name, raw log path, events path, and PTY path
  - rationale: makes router-produced PTYs/logs discoverable without implicit
    filesystem knowledge
  - verified with:
    - `python3 -m py_compile projects/virtioso-camkes-vm/tools/console_router.py`
    - `python3 projects/virtioso-camkes-vm/tools/console_router.py run-command ...`
    - inspection of generated `sessions.json`
- 2026-04-23: Sixth implementation slice completed
  - Autopilot consumer-side session metadata path updated in `/home/hlyytine/autopilot`
  - current scope:
    - `sel4_client.open_console_session()` now returns `pty_path`,
      `events_path`, `interactive`, and `kind` when present
    - `chain_runtime.py` now preserves `pty_path` plus basic session metadata in
      generated `sessions.json`
    - `console_sessions.py` now records `pty_path` into per-session metadata for
      PTY-backed sessions
  - rationale: lets consumers discover router-backed PTYs and richer session
    metadata without special-case filesystem assumptions
  - verified with:
    - `python3 -m py_compile /home/hlyytine/autopilot/sel4_client.py /home/hlyytine/autopilot/console_sessions.py /home/hlyytine/autopilot/chain_runtime.py`
- 2026-04-23: Seventh implementation slice started
  - target slice: land router runtime artifacts directly under Autopilot
    `results/<request>/console/` for QEMU-backed runs
  - current scope:
    - split `qemu_runner.py` ownership between:
      - `--runtime-dir` / `--runtime-tar` for QEMU runtime resolution
      - `--console-runtime-dir` for launcher-owned router artifacts
    - keep remote bundle generation unchanged while making local/remote runner
      wrappers write `console-manifest.json`, `sessions.json`, PTYs, and event
      logs into a caller-selected runtime root
  - rationale: makes router artifacts first-class run outputs instead of temp
    side effects and gives Autopilot a stable handoff point
  - verified with:
    - `python3 -m py_compile projects/virtioso-camkes-vm/tools/qemu_runner.py`
    - `python3 projects/virtioso-camkes-vm/tools/qemu_runner.py run-remote ... --console-runtime-dir /tmp/router-remote-check --dry-run`
- 2026-04-23: Eighth implementation slice completed
  - Autopilot QEMU chain definitions now pass a caller-owned console runtime
    directory to the runner
  - current scope:
    - `qemu_x86_64_defconfig.json` passes
      `--console-runtime-dir {result_dir}/console`
    - `qemu_arm64_defconfig.json` passes
      `--console-runtime-dir {result_dir}/console`
  - rationale: makes router-produced `sessions.json`, PTYs, raw logs, and event
    logs land directly in the canonical result tree without Autopilot needing
    to infer temp paths
  - verified with:
    - JSON parse validation of both chain files
    - diff review showing only the new `--console-runtime-dir` arguments
- 2026-04-23: Ninth implementation slice completed
  - end-to-end proof on a real Autopilot `qemu_x86_64_defconfig` run
  - request id: `20260423-222133`
  - current scope:
    - clean rebuild completed via workspace-root flow:
      - `make mrproper`
      - `make qemu_x86_64_defconfig`
      - `make vm_qemu_virtio`
    - Autopilot result tree now contains router-owned console artifacts under
      `results/20260423-222133/console/`
    - verified artifacts include:
      - `console-manifest.json`
      - `console-runtime/runtime-manifest.json`
      - `console-runtime/sessions.json`
      - per-channel `raw.log`
      - per-channel `events.jsonl`
      - PTY symlink for interactive channel
  - rationale: proves the runner/Autopilot handoff in the canonical QEMU-backed
    workflow instead of only in dry runs or local smoke tests
  - observed runtime note:
    - the run reached `VIRTIOSO_INIT` and `INIT: version 3.14 booting`
    - it did not yet reach `driver-vm login:`
    - the visible stall is guest-side (`CE: Reprogramming failure. Giving up`),
      not a console-artifact plumbing failure
- 2026-04-23: Ninth implementation slice completed
  - `console_router.py` now supports a framed multi-channel transport in
    addition to the legacy one-channel `process_stdio` wrapper
  - current scope:
    - new `transport.type=jsonl_frames` path for `run-command`
    - one wrapped process stdout stream can carry multiple logical channels
    - router now splits those frames into per-channel raw logs, event logs, and
      PTY metadata using the existing manifest/session model
    - interactive input is now modeled against a manifest-defined default input
      channel for framed transports
  - rationale: gives the console stack a real host-side demux path before the
    seL4/QEMU producers are converted to emit framed source-tagged traffic
  - verified with:
    - `python3 -m py_compile projects/virtioso-camkes-vm/tools/console_router.py`
    - smoke run with a two-channel `jsonl_frames` manifest producing distinct
      `driver_vm_console` and `user_vm_console` artifacts
- 2026-04-23: Tenth implementation slice completed
  - `qemu_runner.py` now recognizes `vm_qemu_virtio` as a distinct logical
    console profile instead of only a generic merged-console case
  - current scope:
    - default manifest remains backward-compatible `process_stdio` with
      `merged_console`
    - `vm_qemu_virtio` manifests now declare their intended successor channel
      set even before producer-side framing is enabled
    - explicit opt-in via `VIRTIOSO_CONSOLE_ROUTER_USE_JSONL_FRAMES=1` makes the
      runner emit a framed multi-channel manifest for `vm_qemu_virtio`
  - rationale: moves producer-side ownership forward without breaking the
    current unframed x86/arm runner path
  - verified with:
    - default manifest generation for `vm_qemu_virtio`
    - opt-in manifest generation yielding
      `driver_vm_console` / `driver_vm_control` / `nested_qemu_control` /
      `user_vm_console` / `trace_control`
- 2026-04-23: Eleventh implementation slice completed
  - `qemu_runner.py` now has a producer-side serial-policy override seam for
    generated `simulate` launches
  - current scope:
    - explicit `VIRTIOSO_QEMU_SIM_SERIAL_OPT` passthrough to `./simulate --serial`
    - `vm_qemu_virtio`-specific shortcut `VIRTIOSO_QEMU_SPLIT_MONITOR=1`
      switches generated launches from `-serial mon:stdio` to
      `-serial stdio -monitor none`
    - applies to both local and remote `simulate`-based launches
  - rationale: removes one avoidable source of stream contamination before
    deeper producer-side framing work exists
  - verified with:
    - `python3 -m py_compile projects/virtioso-camkes-vm/tools/qemu_runner.py`
    - dry-run command inspection showing `--serial '-serial stdio -monitor none'`
      on `vm_qemu_virtio` when the split-monitor opt-in is enabled
- 2026-04-24: Twelfth implementation follow-up started
  - first real `qemu_x86_64_vm_qemu_virtio_uservm` run exposed a host-side
    remote-launch bug before VM1 lifecycle logic was exercised
  - concrete failure:
    - `run_remote()` built `ssh ... bash -lc <script>` as separate argv items
    - `ssh` flattened those into one remote command line, so `bash -lc`
      received only `set` as the command string and printed shell state instead
      of running the bundle wrapper
  - action:
    - quote the entire remote script as one remote shell command for both
      `ssh-run` and `ssh-cleanup`
  - rationale:
    - this is required for the managed user-vm chain to test `uservmctl`
      rather than failing in host-side launcher glue
- 2026-04-24: Thirteenth implementation slice started
  - target slice: stop the x86 managed `vm_qemu_virtio` lane from matching the
    merged legacy `tty0` stream
  - current scope:
    - expose router-produced channels as readable sources to Autopilot
    - let `send_cmd` target router-backed channels directly through their PTYs
    - switch the x86 managed chain to `driver_vm_console`
    - enable `VIRTIOSO_CONSOLE_ROUTER_USE_VM_PREFIX_DEMUX=1` for that chain
      so the existing `[vm0]` / `[vm1]` annotations become separable sources
  - rationale:
    - the managed user-vm lane should fail or pass based on VM0 shell and VM1
      lifecycle behavior, not on unrelated VM1/VMM noise in the merged tty log
- 2026-04-24: Thirteenth slice refined after first split-source run
  - observed behavior:
    - prefix demux produced the expected channel files
    - but `driver_vm_console` stayed empty while `vmm_mux_control` absorbed the
      full stream
  - concrete finding:
    - this x86 stream does not present driver-vm guest output as `[vm0] ...`
    - the actual driver-vm guest console is currently the unprefixed
      compatibility stream
    - `vm0:` / `vm1:` prefixes are VMM annotations and should be treated as
      control noise for this lane
  - action:
    - route `vm0:` / `vm1:` and `[vm0]` / `[vm1]` prefixes to
      `vmm_mux_control`
    - route unprefixed fallback lines to `driver_vm_console`
- 2026-04-24: Thirteenth slice refined again for prompt visibility
  - observed behavior:
    - `driver_vm_console` contained boot text
    - but prompts without trailing newline, such as `driver-vm login:`, could
      remain buffered and never reach the channel log
  - action:
    - flush unprefixed fallback fragments incrementally to
      `driver_vm_console`
    - keep prefixed `vm0:` / `vm1:` control lines line-buffered so prefix-based
      classification remains stable
- 2026-04-23: Twelfth implementation slice completed
  - added an intermediate legacy-demux transport for VM-prefixed merged serial
    streams
  - current scope:
    - `console_router.py` now supports `transport.type=line_prefixes`
    - ANSI-stripped line prefixes can be mapped onto logical channels
    - `qemu_runner.py` can emit a `vm_qemu_virtio` prefix-demux manifest with
      `VIRTIOSO_CONSOLE_ROUTER_USE_VM_PREFIX_DEMUX=1`
    - documented legacy prefixes `[vm0] ` and `[vm1] ` now map to
      `driver_vm_console` and `user_vm_console` respectively, with unclassified
      lines routed to `vmm_mux_control`
  - rationale: provides a practical bridge between today's merged VM serial
    stream and the later fully framed source-tagged transport
  - verified with:
    - `python3 -m py_compile projects/virtioso-camkes-vm/tools/console_router.py projects/virtioso-camkes-vm/tools/qemu_runner.py`
    - smoke demux of a prefixed merged stream into distinct per-channel router
      artifacts
- 2026-04-23: Twelfth slice corrected
  - the `[vm0]` / `[vm1]` prefix path is now explicitly treated as legacy
    VMM-serial-mux output classification, not as a guest-native source
    contract
  - current scope:
    - added dedicated `vmm_mux_control` logical channel for mux-side and
      otherwise unclassified legacy stream traffic
    - prefix-demux manifests now record `producer_kind=legacy_vmm_serial_mux`
    - documentation and manifest notes now state that prefix classification is
      only an intermediate compatibility transport
- 2026-04-23: x86 boot regression finding recorded
  - request `20260423-223956` proved the console-router path was not the
    blocker; the guest kernel stalled before `/init`
  - compared with the last good x86 boot (`20260423-220157`), the
    `nolapic_timer` removal regressed boot progression after restoring the
    `raid6_select_algo` blacklist
  - concrete divergence:
    - good run continues through `acpi_button_driver_init`, prints both power
      and sleep button probes, returns from that initcall, and later reaches
      `Run /init as init process`
    - bad run stops inside `acpi_button_driver_init` immediately after the
      power-button line and never reaches the sleep-button probe or `/init`
  - action:
    - restore `nolapic_timer` while keeping
      `initcall_blacklist=raid6_select_algo`

## Problem Statement

We need to satisfy all of these at once:

1. identify which guest or component produced each console byte
2. preserve interactive terminal semantics for humans and automation
3. reconstruct a merged cross-source timeline with input and output events
4. support remote QEMU and hardware-backed targets
5. avoid introducing a proprietary terminal protocol as the user-facing API

The current state already contains useful building blocks:

- named Autopilot sources via `map_source` and `map_command_source`
- bidirectional session logging in `console_sessions.py`
- logical per-VM console seams in the VMM code

But the current state does not yet provide:

- stable source-specific QEMU chardev endpoints
- a single capture authority before PTY exposure
- capture-grade timestamps and sequence numbers
- a merged run timeline artifact

## Verified Current State

### QEMU Runner And Autopilot

- QEMU-backed runs are launched through the runner as a mapped command source:
  [qemu_x86_64_defconfig.json](/home/hlyytine/autopilot/chains/qemu_x86_64_defconfig.json:1),
  [qemu_arm64_defconfig.json](/home/hlyytine/autopilot/chains/qemu_arm64_defconfig.json:1)
- local arm64 fallback currently uses `-serial mon:stdio`, which collapses
  monitor and serial traffic early:
  [qemu_runner.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/qemu_runner.py:53)
- the runner is therefore already the correct architectural seam for QEMU-backed
  console refactoring:
  [qemu_runner.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/qemu_runner.py:1)

### Autopilot Source And Session Model

- Autopilot already has a source-binding abstraction that owns a serial device
  or subprocess:
  [chain_runtime.py](/home/hlyytine/autopilot/chain_runtime.py:165)
- interactive sessions already support bidirectional logging and command
  round-trips:
  [console_sessions.py](/home/hlyytine/autopilot/console_sessions.py:48)
- current event logs are structurally useful but timestamp precision is too weak
  for reliable ordering:
  [console_sessions.py](/home/hlyytine/autopilot/console_sessions.py:109)

### VM Console Identity

- x86 VMM wiring already distinguishes VM-originated guest console traffic from
  component/VMM logging traffic:
  [projects/vm/components/VM/configurations/vm.h](/home/hlyytine/tii-sel4/projects/vm/components/VM/configurations/vm.h:92)
- Arm virtual serial wiring already distinguishes per-VM console paths:
  [projects/vm/components/VM_Arm/configurations/vm.h](/home/hlyytine/tii-sel4/projects/vm/components/VM_Arm/configurations/vm.h:147)

This means the architectural deficiency is backend routing and capture, not a
lack of logical source identity inside the VM system.

## Target Architecture

## Core Principle

Each logical console source gets one authoritative router instance. That router:

1. receives bytes from the source
2. timestamps and labels them
3. appends structured event records
4. forwards the same raw bytes to a PTY
5. accepts input written to that PTY
6. timestamps that input too
7. forwards input upstream to the original source

There is one byte path, not two competing ones.

## Channel Manifest

The demultiplexer must support an arbitrary number of logical channels, but it
should not discover topology by guessing from traffic. The channel set for a run
should come from a launcher-generated manifest.

Manifest ownership:

- QEMU-backed runs: generated by `qemu_runner.py`
- hardware-backed runs: generated by the target launcher or bench wrapper
- Autopilot: consumer only, not the source of truth

Minimum manifest responsibilities:

- declare the transport type and parameters
- declare the logical channel set for the run
- map compact transport ids to stable logical names
- declare whether a channel is interactive, log-only, or both

Illustrative shape:

```json
{
  "version": 1,
  "run_id": "2026-04-23T12:34:56Z",
  "transport": {
    "type": "uart_mux",
    "path": "/dev/ttyACM0",
    "baud": 115200
  },
  "channels": [
    {
      "id": 1,
      "name": "camkes_log",
      "kind": "log",
      "interactive": false,
      "pty": false
    },
    {
      "id": 2,
      "name": "vm0_console",
      "kind": "guest_console",
      "interactive": true,
      "pty": true
    },
    {
      "id": 3,
      "name": "vm1_console",
      "kind": "guest_console",
      "interactive": true,
      "pty": true
    }
  ]
}
```

This keeps the demux generic while keeping naming and authority tied to the
actual launched topology.

## Logical Sources

Initial target source set:

- `host_boot`
- `camkes_log`
- `vm0_console`
- `vm1_console`
- `nested_qemu_vm1_console` where applicable

Not every target needs every source, but the source naming contract should be
stable across backends where possible.

## User-Facing Contract

Humans and generic tools see:

- PTYs
- raw terminal behavior

Automation and analysis see:

- per-source event logs
- a merged run timeline

The PTY is the interaction API. The event logs are the analysis API.

## Event Schema

Each captured event should record at least:

- `source`
- `dir` (`rx` or `tx`)
- `clock_monotonic_ns`
- `clock_realtime_ns`
- `seq`
- `payload_b64`
- `payload_len`
- optional normalized text fields
- optional session/operator metadata

The raw payload must remain primary. Text decoding is supplemental.

## Ownership Boundaries

### QEMU Runner

Responsible for:

- configuring QEMU chardev topology
- generating the run manifest for QEMU-backed targets
- exposing distinct raw endpoints where available
- launching source routers or host-side routing helpers

Not responsible for:

- prompt matching policy
- analysis decisions
- higher-level test verdicts

### Source Router

Responsible for:

- byte capture
- timestamps
- sequence numbering
- PTY forwarding
- event log persistence

Not responsible for:

- semantic interpretation of prompts
- VM boot policy

### Autopilot Runtime

Responsible for:

- source registration and lifecycle
- pattern matching against chosen logical sources
- opening interactive sessions on PTYs
- producing merged timeline artifacts

Not responsible for:

- low-level QEMU chardev ownership
- declaring console topology
- guest-side console generation

## Implementation Plan

## Phase 0: Freeze The Model In Documentation

Goal:

- establish the source-router architecture as the target contract before code
  changes spread across runner and Autopilot

Deliverables:

- this plan
- update any stale console architecture notes later to point here

Validation:

- architecture review by humans working on QEMU runner and Autopilot

Rollback:

- none needed

## Phase 1: Introduce A Formal Source Router Abstraction In Autopilot

Goal:

- normalize "source" around a richer contract than today's plain UART or
  subprocess capture

Scope:

- extend `SourceBinding` in [chain_runtime.py](/home/hlyytine/autopilot/chain_runtime.py:165)
- keep existing `map_source` and `map_command_source` working
- add support for event-rich capture without changing current chains yet

Changes:

- add monotonic nanosecond timestamps
- add per-source sequence numbers
- add event-log writer separate from the raw log file
- preserve current raw `.log` / `.raw` outputs for compatibility

Artifacts:

- `console/<source>.raw` or `.log`
- `console/<source>.events.jsonl`

Validation:

- existing Autopilot chains still run unchanged
- one test source records `rx` bytes to both raw log and event log

Rollback:

- event log emission can be gated by a feature flag or default-off option

## Phase 2: Capture TX Symmetrically

Goal:

- ensure operator and automation input become first-class timeline data

Scope:

- [console_sessions.py](/home/hlyytine/autopilot/console_sessions.py:1)
- interactive command path in [sel4_client.py](/home/hlyytine/autopilot/sel4_client.py:648)

Changes:

- upgrade existing `tx` event logging to the new schema
- ensure input is timestamped at the router boundary, not later in higher-level
  command handling
- include a stable session or actor identifier when available

Validation:

- open a console session
- send a known command
- verify event order: `tx` command then `rx` response with ns timestamps

Rollback:

- retain old `log` byte-offset reading flow

## Phase 3: Stop Merging QEMU Sources Onto `stdio` By Default

Goal:

- expose distinct source endpoints from outer QEMU rather than recovering
  meaning from merged output

Scope:

- [qemu_runner.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/qemu_runner.py:1)

Changes:

- replace `-serial mon:stdio` style fallback/default where source separation is
  required
- configure explicit QEMU chardev endpoints per logical output where supported
- prefer endpoint forms that are easy for a host-side router to own

Initial target:

- `qemu_arm64_defconfig`
- `qemu_x86_64_defconfig` if separate endpoints are available from the existing
  launch topology

Validation:

- prove that outer QEMU can expose at least one guest console without collapsing
  it into the runner stdout
- prove that monitor traffic and guest console traffic are no longer merged by
  default

Rollback:

- preserve a compatibility mode that still runs through merged stdout

## Phase 4: Add Host-Side Inline Router Process For QEMU Sources

Goal:

- make one host-side process the authoritative capture point before PTY exposure

Scope:

- likely new helper under `projects/virtioso-camkes-vm/tools/` or Autopilot code
- runner integration to spawn/manage it

Responsibilities of the router:

- connect to QEMU chardev endpoint or subprocess stdout source
- create PTY pair
- forward source bytes to PTY master/slave
- accept PTY input and send upstream
- emit event logs with timestamps and sequence numbers

Why host-side first:

- it works for QEMU without touching guest or VMM code
- it can later serve hardware UART and framed single-UART backends too
- it avoids coupling the architecture too tightly to one QEMU invocation style

Validation:

- attach `minicom` or equivalent to the PTY
- interact with the guest successfully
- verify same bytes appear in the event log

Rollback:

- keep direct runner stdout mode available during transition

## Phase 5: Add Merged Timeline Generation

Goal:

- produce a run-wide reconstruction of all observed console activity

Scope:

- Autopilot analysis hook or new script

Inputs:

- `console/*.events.jsonl`

Output:

- `console/timeline_merged.jsonl`

Merge key:

1. `clock_monotonic_ns`
2. `source`
3. `seq`

Notes:

- this does not claim exact guest-internal causal order
- it defines exact capture-boundary order, which is the correct operational
  standard for this system

Validation:

- inject known interleavings across two or more sources
- confirm stable merged order across repeated runs

Rollback:

- event logs remain useful even before merged timeline generation is perfect

## Phase 5.5: Introduce Manifest-Driven Demux Runtime

Goal:

- make the channel topology explicit and backend-owned rather than inferred by
  individual consumers

Scope:

- `qemu_runner.py` manifest generation for QEMU-backed runs
- host-side demux/router runtime
- Autopilot manifest consumption

Changes:

- generate `console-manifest.json` for each run
- require demux/router startup from that manifest
- resolve transport channel ids through the manifest instead of hardcoded source
  names in the demux

Responsibilities of the demux:

- load manifest
- create PTYs for interactive channels
- create raw and event logs for all channels
- route inbound and outbound bytes using manifest-defined channel ids

Validation:

- one manifest can drive a 2-channel or 5-channel run with no code change in
  the demux
- Autopilot can consume the same manifest and expose sessions by logical name

Rollback:

- legacy source mapping can continue to coexist while manifest support is phased
  in

## Concrete Use Case: `vm_qemu_virtio`

The clearest immediate use case is the existing two-VM `vm_qemu_virtio`
workflow.

Current operational shape:

- VM0 boots first and reaches initramfs/userspace
- automation or an operator may need to log into `driver-vm`
- `qemu-rnd-helper` is then launched inside VM0
- that helper launches nested QEMU for the user VM
- user-VM boot markers and later console traffic then appear in the same
  observed path unless they are separated explicitly

Why the current model is insufficient:

- one physical/logical source is being used to represent multiple phases and
  speakers
- Autopilot chains are then forced to distinguish VM0 login, helper lifecycle,
  nested QEMU output, and user-VM readiness by grepping one contaminated byte
  stream
- this makes pass/fail logic fragile and order-dependent
- it also hides whether a matched marker belongs to the driver VM shell, a
  helper control message, or the user VM itself

The router/demux target for this workflow is:

- `driver_vm_console`
- `driver_vm_control`
- `nested_qemu_control`
- `user_vm_console`
- optional `trace_control`

Those exact names may still change, but the architectural requirement is now
explicit: `vm_qemu_virtio` needs multiple named logical streams, not one mixed
tty log.

What that enables:

- wait for VM0 boot/login on the driver-VM stream
- send login or helper commands through the right interactive/control seam
- wait for nested-QEMU launch markers on the helper/control stream
- wait for user-VM readiness on the user-VM console stream
- keep all of those as first-class timeline sources instead of text heuristics
  over one merged log

## Phase 6: Switch Autopilot Pattern Matching To Logical Sources

Goal:

- stop building chain semantics around merged physical TTY assumptions when
  source-specific channels exist

Scope:

- selected chain JSON files
- documentation and runbooks

Changes:

- use logical sources such as `vm0_console` or `vm1_console` instead of
  overloading `tty0` or `tty1`
- reserve physical `tty*` naming for true hardware/UART sources
- for `vm_qemu_virtio`, stop expressing VM0 login, helper launch, and user-VM
  readiness as regexes over one mixed stream; bind each wait to the correct
  logical source instead

Validation:

- existing pass/fail markers still work
- vm-specific waits stop depending on mixed streams

Rollback:

- dual-source matching can be supported temporarily if needed

## Phase 7: Integrate VM1 Launch Automation With Logical Console Sources

Goal:

- use the new console model to support `vm1` / user-vm launch and readiness
  validation cleanly

Scope:

- future `uservmctl` or equivalent launcher contract
- Autopilot chain updates

Changes:

- launch VM1 from driver-vm via explicit command surface
- observe readiness on `vm1_console`, not on mixed shell output
- keep benchmark/operator PTY access intact

Validation:

- automated launch of VM1
- readiness wait based on the right logical source
- manual attach to the same PTY remains possible

Rollback:

- retain direct shell launch fallback during migration

## Phase 8: Add Optional Framed Single-UART Backend For Hardware

Goal:

- support hardware targets where many physical UARTs are unavailable

Scope:

- separate transport backend, not a user-facing API

Changes:

- introduce a dedicated seL4/CAmkES-side mux component, conceptually
  `ConsoleMuxServer`, that owns the single uplink transport
- connect guest consoles and selected log producers to that mux as logical
  channels rather than wiring them directly to the physical UART
- multiplex multiple logical sources onto one physical UART with framed packets
- demux on host side into:
  - per-source PTYs
  - per-source event logs

Important rule:

- framing terminates before PTY exposure
- users and tools still see PTYs, not the mux protocol

Validation:

- single-UART hardware can reproduce source-separated PTYs and merged timeline
- same Autopilot logic works against demuxed logical sources

Muxer-side architectural rules:

- the muxer is a dedicated CAmkES component, not ad hoc logic spread across VM
  components
- the muxer is a framed byte router, not a terminal emulator and not a prompt
  matcher
- the channel set is configured declaratively at build/launch time
- the host-side manifest and the seL4-side channel table must describe the same
  logical channels and transport ids

Initial local channel classes:

- bidirectional guest console channels
- write-only log channels
- optional future control/meta channels

Rollback:

- only enable on targets that need it

## Phase 9: Optional VMM-Side Source Refinement

Goal:

- improve identity of CAmkES/VMM/component logs if host-side separation alone is
  insufficient

Scope:

- `projects/vm`
- target app-level log routing

Changes:

- add clearer source identifiers for VMM/component-originated output
- possibly route specific component logs to dedicated logical channels
- allow `ZF_LOG` sinks to target mux-managed log channels rather than collapsing
  into the same sink as guest serial output

Guiding rule:

- `ZF_LOG` is a supported producer class, but not the design center of the mux
- the mux exposes generic write-only log channels
- `ZF_LOG` adapters bind components or subsystems onto those channels

Initial coarse log-channel set:

- `camkes_log`
- `vm0_vmm_log`
- `vm1_vmm_log`

Validation:

- `camkes_log` is distinguishable from guest consoles without text heuristics

Rollback:

- keep textual prefixes until a cleaner channel model is proven

## Non-Goals

This plan does not require:

- replacing PTYs with a custom console client protocol
- changing guest kernels just to achieve source capture
- moving nested VM launch out of driver-vm
- making every target use many physical UARTs

## Recommended First Implementation Slice

The narrowest useful slice is:

1. extend Autopilot event schema to ns timestamps and per-source sequence numbers
2. add one host-side source router for one QEMU-backed source
3. expose one PTY from that router
4. prove that:
   - interactive terminal use still works
   - event log is correct
   - merged ordering across at least two sources is reconstructable

Why this slice first:

- it proves the ownership model
- it does not require immediate VMM or guest changes
- it gives immediate value to debugging and AI-assisted operation

## Validation Matrix

### Functional Validation

- PTY attach works with `minicom` or similar
- AI console session can send commands and read output
- event logs contain both `rx` and `tx`

### Attribution Validation

- bytes from `vm0_console` do not appear under `vm1_console`
- monitor or launcher output does not contaminate guest console sources

### Timeline Validation

- merged event file is stable and monotonically ordered
- simultaneous activity from two sources is reconstructable

### Regression Validation

- existing chains still pass under compatibility mode
- legacy raw logs remain available during migration

## Risks

- QEMU endpoint topology may vary between arm64 local and x86 remote backends
- if QEMU still collapses key streams too early, runner changes are mandatory
- PTY forwarding bugs can damage interactivity if the router is careless about
  buffering and terminal semantics
- event-log volume may become large; JSONL is fine initially but may need a more
  compact format later

## Open Questions

- exactly which distinct QEMU chardev endpoints are available in each target
  launch topology after runner refactoring
- whether `camkes_log` should be a dedicated source immediately or a later
  refinement
- whether nested user-vm console should be modeled as `vm1_console` or a more
  explicit `nested_qemu_vm1_console` depending on target topology

## Decision

Proceed with a single inline capture-and-forward path per source.

Do not build two independent byte transports for "interactive" and
"observation". Instead:

- one router owns the source
- PTY is the interactive downstream interface
- structured events are emitted from that same path

That is the simplest model that preserves terminal usability while making
timeline reconstruction and source attribution reliable.
