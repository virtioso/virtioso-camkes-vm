# Console Timeline And Interactive Architecture

Date: 2026-04-23

## Summary

The console architecture needs to satisfy two goals that pull in different
directions:

- preserve interactive terminal semantics for humans and tools such as
  `minicom`
- reconstruct an exact cross-source timeline of output and input events

Those goals should not be forced into one stream format. The recommended
architecture is a dual-plane model:

1. raw byte channels for interactive use
2. timestamped event records for observation and analysis

This means VM consoles should still be exposed as PTYs or equivalent raw
streams, but every byte crossing those channels should also be mirrored into a
structured event log with a common capture clock and source identity.

## Current State

Verified seams:

- Autopilot already models consoles as named sources and persists per-source
  JSONL logs under `results/<timestamp>/console/`:
  docs/overview.md,
  chain_runtime.py
- Autopilot interactive console sessions already record bidirectional events
  (`rx` and `tx`) per session:
  console_sessions.py
- Those event logs currently use wall-clock timestamps with second precision:
  console_sessions.py
- The current VMM code already distinguishes per-VM guest console paths from
  component/VMM logging paths:
  projects/vm/components/VM/configurations/vm.h,
  projects/vm/components/VM_Arm/configurations/vm.h

So the missing architecture is not "console sources exist." The missing
architecture is:

- a capture-grade timestamp model
- a unified event schema
- a clean separation between interactive transport and analytical timeline

## Recommendations

## Recommendation 1: Keep raw PTY-style channels as the interactive contract

Rationale:

- Terminal emulators expect an unframed byte stream.
- Guest consoles should remain attachable by generic tools.
- Input must work without a protocol-aware terminal on the reader side.

Affected areas:

- remote QEMU backends
- Autopilot source/session plumbing
- future hardware demux host-side adapters

Short-term benefit:

- `minicom`, `screen`, `picocom`, or custom AI tooling can attach directly
- no decoder is required for normal interaction

Migration implication:

- PTY exposure becomes the stable operator/dev interface
- existing UART-backed flows can be wrapped behind PTYs without changing users

## Recommendation 2: Introduce a capture event plane alongside raw streams

Each source should emit structured events on ingress/egress, for example:

- `source`
- `direction` (`rx` or `tx`)
- `clock_monotonic_ns`
- `clock_realtime_ns`
- `seq`
- `payload_b64`
- `payload_len`
- optional `session_id`
- optional `decode` fields for normalized text

Rationale:

- exact cross-source ordering requires a common clock at the capture point
- per-source logs alone are not enough unless their timestamps are comparable
- raw payload must be preserved to avoid lossy text decoding

Affected areas:

- console_sessions.py
- chain_runtime.py
- result artifact schema under `results/<timestamp>/console/`

Short-term benefit:

- merged timeline reconstruction becomes deterministic enough for debugging and
  AI analysis
- input actions become part of the same trace as output

Migration implication:

- existing `.jsonl` can be extended rather than replaced
- raw `.log` or PTY byte streams remain the compatibility layer

## Recommendation 3: Timestamp at the router/capture boundary, not in the guest

Rationale:

- guest timestamps are useful but cannot order events across sources reliably
- the only trustworthy merge point is where the system actually captures bytes
- this avoids depending on guest wall clocks, NTP state, or boot phase

Recommended policy:

- assign `clock_monotonic_ns` as close as possible to the read/write syscall
- also store `clock_realtime_ns` for human correlation
- maintain a per-source sequence number for tie-breaking and gap detection

Short-term benefit:

- exact ordering improves immediately without guest changes

Migration implication:

- this is additive and local to tooling/backends

## Recommendation 4: Do not make the consumer decode frames to interact

Rationale:

- the reader side should not need a special terminal emulator to talk to VM1
- framing is useful for transport or archival, not for the everyday terminal API

Therefore:

- interactive plane: raw PTY
- event plane: structured JSONL or binary event file

If a single physical UART transport must carry multiple sources, the mux layer
should terminate before PTY exposure on the host side. The demux daemon should:

- decode the framed transport
- recreate per-source PTYs
- emit the structured event log

This keeps ordinary tools talking to PTYs, not to a proprietary framed stream.

## Recommendation 5: Treat tx events as first-class timeline data

Rationale:

- to replay or understand system behavior, operator and AI inputs matter as much
  as guest output
- "why did this prompt disappear" often depends on prior input timing

Short-term benefit:

- exact reproduction of test interactions
- better debugging of prompt matching and automation errors

Migration implication:

- current `rx`/`tx` event model in Autopilot is the right base, but precision and
  schema need to be improved

## Recommended Target Shape

For each logical source, expose:

- raw PTY or raw byte stream endpoint for interaction
- structured append-only event log for analysis

For each run, produce:

- `console/<source>.log` or PTY transcript for compatibility
- `console/<source>.events.jsonl` with capture timestamps
- `console/timeline_merged.jsonl` generated by stable merge on:
  - `clock_monotonic_ns`
  - `source`
  - `seq`

Example logical sources:

- `camkes_log`
- `vm0_console`
- `vm1_console`
- `host_launcher`

## Validation Path

## Phase 1: Event schema upgrade

Extend current JSONL events to include:

- nanosecond monotonic timestamp
- realtime timestamp
- source
- sequence number
- raw payload encoding

Validation:

- open one interactive session
- send known commands
- verify `tx` and `rx` order in event log

## Phase 2: PTY-backed source exposure for QEMU

Expose VM consoles as PTYs while mirroring all traffic to event logs.

Validation:

- attach `minicom` or equivalent to VM1 PTY
- interact successfully
- verify same bytes appear in `vm1.events.jsonl`

## Phase 3: Merged timeline generation

Add a merge tool that combines all per-source events into one run timeline.

Validation:

- inject known interleaving across `camkes_log`, `vm0_console`, `vm1_console`
- verify merge order matches observed behavior

## Phase 4: Optional framed single-UART backend

Only for hardware needing one physical transport:

- multiplex logical sources over one UART with framing
- demux on host side into PTYs plus event logs

Validation:

- prove that host-side PTYs behave the same as native separate channels
- prove merged timeline preserves source order and source identity

## Risks and Open Questions

- "Exact" ordering is only exact at the capture boundary, not at the instant the
  guest originally called `printk()`. That is usually good enough, but it should
  be stated explicitly.
- PTY buffering and terminal line discipline can distort human-visible timing if
  capture happens too far from the byte source. Capture should happen before any
  terminal emulator layer.
- Some CAmkES/component logs may currently bypass the same capture infrastructure
  as guest console bytes; they need to be brought under the same event schema.
- JSONL may be sufficient initially, but a binary event format may become
  preferable if log volume grows substantially.
