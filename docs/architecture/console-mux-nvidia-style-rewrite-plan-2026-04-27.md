# Console Mux NVIDIA-Style Rewrite Plan

Date: 2026-04-27

Status: active replacement direction

Cross-architecture successor:

- [cross-arch-tcu-uart-mux-plan-2026-05-01.md](cross-arch-tcu-uart-mux-plan-2026-05-01.md)

Note: the successor keeps the NVIDIA-style stream-switching behavior, but uses
`0xfe` as the local escape byte instead of NVIDIA `tcu_muxer`'s `0xff` so the
local mux can run over a real NVIDIA TCU path.

Supersedes as target architecture:

- [console-transport-and-routing.md](console-transport-and-routing.md)
- [console-mux-camkes-architecture-plan-2026-04-25.md](console-mux-camkes-architecture-plan-2026-04-25.md)
- [../integration/x86-vm-qemu-virtio-framed-console-transport-plan-2026-04-24.md](../integration/x86-vm-qemu-virtio-framed-console-transport-plan-2026-04-24.md)
- [../integration/console-router-and-timeline-implementation-plan-2026-04-23.md](../integration/console-router-and-timeline-implementation-plan-2026-04-23.md)

Reference implementation to follow:

- [`sources/tcu_muxer/uart-proto.h`](/home/hlyytine/tii-sel4/sources/tcu_muxer/uart-proto.h:27)
- [`sources/tcu_muxer/tcu_com.c`](/home/hlyytine/tii-sel4/sources/tcu_muxer/tcu_com.c:696)

## Summary

The current `CF` / `binary_frames` console transport is too complex for the
problem it solves. We own the seL4 side, the host-side demux, and the Autopilot
integration, so there is no compatibility reason to keep the framed transport.

The replacement architecture direction in this note has been refined by the
cross-architecture successor. Read this section as historical rationale unless
the successor agrees.

The replacement architecture is:

- keep the physical second-UART separation so low-level platform output does
  not pollute the muxed stream
- keep producer-owned logical streams on the CAmkES side
- assign stream ids via generated CAmkES enumeration rather than manual
  duplication
- use a minimal NVIDIA-style escape protocol on the muxed UART:
  - in the successor, emit `0xfe <stream-id>` only when the active stream
    changes
  - in the successor, emit `0xfe 0xfe` for a literal `0xfe` payload byte
  - otherwise emit the payload byte unchanged
- use NVIDIA `tcu_muxer` behavior as the host-side demux baseline

The mux protocol is not where logging, PTY management, event journaling, or
interactive policy should live. Those are host-tooling concerns layered above
demuxed streams, not part of the wire format.

## Decision

### Keep

- second UART boundary for non-muxed low-level output
- explicit producer ownership of stream identity
- implicit stream enumeration in CAmkES/app composition
- distinct logical streams such as VM0 console, VM1 console, VMM control, and
  VMM debug

### Remove

- `CF` magic/version/length framing
- `binary_frames` transport mode as an architectural target
- `jsonl_frames` as a target transport
- router-owned transport intelligence
- protocol-level direction fields, flags, and per-record metadata
- any requirement to preserve compatibility with old framed console paths

## Current State

The current design spreads transport semantics across repo-owned target and
host code:

- [`components/GuestConsoleSink/src/guest_console_sink.c`](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/components/GuestConsoleSink/src/guest_console_sink.c:79)
  locally wraps every payload byte in an 11-byte frame
- [`components/ConsoleMux/src/console_mux.c`](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/components/ConsoleMux/src/console_mux.c:42)
  forwards framed payloads and also injects debug/profiling records in that
  same format
- [`tools/console_router.py`](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/console_router.py:530)
  owns multiple transport modes, including `binary_frames`
- [`tools/qemu_runner.py`](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/qemu_runner.py:736)
  selects `binary_frames` transport for the current split-console path

That is the wrong ownership split. The protocol is carrying host-runtime policy
instead of just stream identity.

## Target Architecture

### seL4 / CAmkES Side

Responsibilities:

- own the list of logical streams
- assign each stream a generated numeric id
- serialize producers onto one muxed uplink
- emit the minimal escape protocol

Non-responsibilities:

- per-channel log file semantics
- PTY/session creation
- Autopilot source aliasing
- event-log framing
- direction metadata in the wire format

### Host Side

Responsibilities:

- demux the NVIDIA-style escaped byte stream into logical outputs
- map stream ids to generated stream names
- provide optional PTYs, logs, and runtime manifests for higher-level tools

Non-responsibilities:

- guessing source identity from text prefixes
- decoding `CF` headers
- preserving old `binary_frames` compatibility

### Autopilot Side

Autopilot should consume named post-demux channels only. It should not know
about escape bytes, `CF` frames, or router transport variants.

## Protocol

The muxed UART protocol in this historical note used NVIDIA's exact byte. The
cross-architecture successor keeps the behavior but changes the local escape
byte to `0xfe`.

- `0xfe <stream-id>` switches the active output stream
- `0xfe 0xfe` encodes a literal `0xfe` byte in the payload
- all other bytes are payload for the current active stream

Expected behavior:

- a producer does not repeat its stream tag for every byte
- a stream tag is emitted only when output ownership changes
- payload bytes remain byte-exact except for the `0xff` escape case

This follows the same design level as NVIDIA `tcu_muxer`, not the current local
framed transport.

## Stream Identity

Stream ids should come from one generated source, owned by the app composition.

Rules:

- define the canonical stream list once in CAmkES/app-local metadata
- generate both numeric ids and name mappings from that list
- do not manually duplicate ids in:
  - CAmkES attributes
  - target C code
  - host router code
  - Autopilot configuration

The intended shape is:

1. CAmkES/app composition declares the stream set
2. generated artifacts provide:
   - enum or constants for target-side emitters
   - id-to-name mapping for host-side demux/runtime tooling
3. all higher layers consume those generated names after demux

## Component Boundaries

### Producer Components

Each producer should emit bytes into a mux-facing interface bound to one logical
stream id. The producer should not know about host runtime policy.

Examples:

- VM0 guest console
- VM1 guest console
- VMM control chatter
- VMM debug output

### Mux Component

The mux should be tiny and atomic.

Its job is only:

- shared uplink serialization
- active-stream tracking
- minimal escape emission

It should not:

- build variable-length records
- measure/report RPC profiling on the wire
- invent stream classifications
- carry host-only metadata

### Sink Boundary

The physical UART sink remains a separate boundary. The second UART policy is
preserved so low-level platform software continues to use its own path.

## Host Demux Direction

Host demux should use NVIDIA `tcu_muxer` as the behavioral baseline.

Concretely:

- reuse the escape parsing model from `sources/tcu_muxer`
- configure channel definitions from generated CAmkES stream metadata
- adapt host-side output plumbing around that demux rather than inventing a new
  transport contract

This does not require keeping NVIDIA guest-name query/reply features unless we
decide they are useful. The required baseline is the stream-switch escape
protocol and its demux semantics.

## Migration Plan

### Slice 1: Freeze the New Target

- mark `binary_frames` and `line_prefixes` as legacy or superseded in docs
- establish this file as the new architecture source for mux/demux direction

### Slice 2: Define Generated Stream Inventory

- move the canonical stream list to one app-owned declaration surface
- generate:
  - target-side stream constants
  - host-side stream map metadata

Exit criteria:

- no hand-maintained duplicate stream id tables remain in target and host code

### Slice 3: Replace Target Wire Encoding

- remove `CF` record construction from target producers
- implement NVIDIA-style stream-switch encoding on the muxed UART path
- keep second-UART isolation intact

Exit criteria:

- target emits only escaped stream-switch bytes plus raw payload

### Slice 4: Replace Host Demux

- remove `binary_frames` decode from the active path
- use a `tcu_muxer`-style demux path driven by generated stream metadata
- keep optional per-channel PTY/log/runtime features above the demux seam

Exit criteria:

- active runtime path no longer depends on `console_router.py` binary-frame
  decode logic

### Slice 5: Simplify Autopilot Integration

- consume only post-demux named channels
- remove `binary_frames` transport assumptions from runner/integration logic

Exit criteria:

- Autopilot source selection is independent of wire protocol details

### Slice 6: Delete Legacy Framed Paths

- remove unused `CF` frame code
- remove unused `binary_frames` transport mode
- remove stale docs and diagrams that still present framed transport as target

Exit criteria:

- the repo no longer presents framed transport as the intended architecture

## Validation Path

### Static Validation

- verify a single canonical stream inventory drives both target and host
- verify no remaining active-path code emits or expects `CF` records

### Runtime Validation

- run the split-console path and capture raw muxed UART bytes
- confirm:
  - stream switch emits `0xff <id>`
  - same-stream byte runs do not repeat the tag
  - literal `0xff` becomes `0xff 0xff`
- confirm VM0 and VM1 consoles remain separable after host demux
- confirm low-level platform output remains on the separate UART path

### Performance Validation

- compare payload expansion and call counts against the current framed path
- confirm the new mux path removes the current 11-byte-per-byte framing tax

## Risks And Open Questions

- The generated stream-id source needs a concrete implementation seam in the
  current CAmkES/app pipeline.
- We need to decide whether to wrap `tcu_muxer` directly, fork a minimal local
  variant, or extract only the protocol logic and keep repo-local output
  plumbing.
- Existing router/runtime conveniences such as PTY and event logs may still be
  useful, but they must become transport-agnostic post-demux tooling.
- Some current x86-local stream naming may need cleanup when the generated
  inventory becomes authoritative.

## Immediate Next Actions

1. Audit and list every active-path dependency on `CF`, `binary_frames`, and
   `line_prefixes`.
2. Define the concrete generated stream inventory seam in CAmkES/app metadata.
3. Replace target emission first, then replace host demux, then remove legacy
   framed transport code.
