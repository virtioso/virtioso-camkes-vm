# Console Transport And Routing Architecture

Superseded target note:

- [cross-arch-tcu-uart-mux-plan-2026-05-01.md](cross-arch-tcu-uart-mux-plan-2026-05-01.md)

This document is retained as historical inventory for the April 2026 x86
console-router work. It is not the target architecture for mux/demux transport.
`line_prefixes`, `binary_frames`, and `CF` framing are local-only post-upstream
work and may be rewritten or deleted. The current target is the forked
TCU-style `0xfe` escape protocol described in the superseding note.

This document was the source of truth for the April 2026 console
muxer/demuxer architecture in `projects/virtioso-camkes-vm`.

It covers:

- the logical channel model
- transport modes and their authority boundaries
- the router runtime contract
- current x86 `qemu_x86_64` + `vm_qemu_virtio` implementation status

Autopilot integration is described separately in
[autopilot-console-source-integration.md](autopilot-console-source-integration.md).

Visual topology companions:

- [console-stream-topology-diagrams.md](console-stream-topology-diagrams.md)
- [console-stream-topology-binary-frames.svg](console-stream-topology-binary-frames.svg)
- [console-stream-topology-line-prefixes.svg](console-stream-topology-line-prefixes.svg)
- [console-stream-topology-autopilot-io.svg](console-stream-topology-autopilot-io.svg)

## Summary

The console system has three layers:

1. a producer emits console bytes
2. `tools/console_router.py` turns that producer stream into named channel
   runtimes
3. consumers such as Autopilot read and write those named channels

The architectural rule is simple: source identity should be owned by the
producer, not inferred by the router. `binary_frames` is the target
architecture. `line_prefixes` exists only as a compatibility bridge for older
merged plaintext producers.

## Logical Channel Model

The current canonical channel set for the x86 `vm_qemu_virtio` profile is
declared in [tools/qemu_runner.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/qemu_runner.py:593).

| Id | Name | Kind | Interactive | Purpose |
| --- | --- | --- | --- | --- |
| `1` | `driver_vm_console` | `guest_console` | yes | VM0 login, shell, and default interactive input |
| `2` | `driver_vm_control` | `control` | yes | reserved VM0 control lane |
| `3` | `vmm_mux_control` | `control` | no | legacy `vm0:` / `vm1:` mux/control chatter |
| `4` | `nested_qemu_control` | `control` | no | reserved nested-QEMU control lane |
| `5` | `user_vm_console` | `guest_console` | yes | intended VM1 guest console |
| `6` | `trace_control` | `trace` | no | reserved trace/control lane |
| `7` | `vmm_debug` | `debug` | no | dedicated VMM heartbeat and progress stream |

Operational meanings:

- `driver_vm_console` is the verdict-bearing VM0 interactive stream
- `tty0` is not a real producer-owned channel; it is a compatibility alias or
  runner-facing source name depending on transport
- `vmm_mux_control` and `vmm_debug` are different seams and must not be treated
  as aliases

## Ownership Layers

### Producer

The producer is whatever emits bytes into the router-managed transport stream.

Current producer shapes:

- plain process stdio from `qemu_runner.py`
- legacy merged plaintext with VMM textual prefixes
- native framed output from the x86 `Init` component

In the target architecture, the producer decides channel identity explicitly.

### Router

[tools/console_router.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/console_router.py:530)
is the executable routing boundary.

It is responsible for:

- loading `console-manifest.json`
- validating the declared channels and transport
- creating `console-runtime/`
- creating per-channel `raw.log` and `events.jsonl`
- creating PTYs for interactive channels
- emitting `runtime-manifest.json` and `sessions.json`
- dispatching producer `rx` bytes and consumer `tx` bytes

It should not own source inference in the steady-state architecture.

### Consumer

Consumers use named channels after the router has established them.

Current consumers:

- Autopilot chain runtime
- humans reading per-channel logs
- direct PTY writers targeting interactive channels

## Transport Modes

Transport selection is declared by
[tools/qemu_runner.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/qemu_runner.py:633)
in `console-manifest.json`.

### `process_stdio`

Characteristics:

- one merged runner-owned stream
- no source-specific demux
- backward-compatible default

Authority model:

- the runner owns one channel, typically `merged_console`
- consumers cannot distinguish producers beyond that

Use:

- legacy and non-split flows

### `line_prefixes`

Characteristics:

- router classifies merged plaintext by prefix
- unmatched text falls through to a configured fallback channel
- line-oriented, with special handling for partial fallback fragments

Current prefix map for x86 `vm_qemu_virtio`:

- `[vmmdbg] ` or `vmmdbg: ` -> `vmm_debug`
- `[vm0] `, `[vm1] `, `vm0: `, `vm1: ` -> `vmm_mux_control`
- everything else -> `driver_vm_console`

Implementation references:

- manifest declaration:
  [tools/qemu_runner.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/qemu_runner.py:675)
- classification and fallback flush:
  [tools/console_router.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/console_router.py:220),
  [tools/console_router.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/console_router.py:251)

Authority model:

- source identity is guessed by the router after merging
- verdict quality depends on prefixes, formatting, and newline timing

Status:

- compatibility-only
- not acceptable as the target architecture

### `jsonl_frames`

Characteristics:

- producer emits explicit channel-tagged JSONL frames
- router decodes frames instead of classifying plaintext

Authority model:

- producer owns channel identity
- router is a decoder and dispatcher

Status:

- representable in code
- not the current x86 steady-state path

### `binary_frames`

Characteristics:

- producer emits length-delimited binary frames
- each frame contains `magic`, `version`, `stream_id`, `direction`, `flags`,
  `payload_length`, and `payload`

Implementation references:

- manifest declaration:
  [tools/qemu_runner.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/qemu_runner.py:645)
- router encode/decode:
  [tools/console_router.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/console_router.py:200)
- transitional single-channel wrapper:
  [tools/console_frame_stream.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/console_frame_stream.py:1)

Authority model:

- producer owns stream identity
- router only decodes and dispatches
- no textual prefix heuristics are required for framed payloads

Status:

- target architecture
- partially implemented end to end on x86

## Router Runtime Contract

The router runtime layout is created under `console-runtime/`.

Files:

- `runtime-manifest.json`: resolved runtime view of the manifest
- `sessions.json`: channel-to-session map used by consumers
- `channels/<name>/raw.log`: raw byte log for that channel
- `channels/<name>/events.jsonl`: `rx` / `tx` event log for that channel
- `channels/<name>/pty`: PTY symlink for interactive channels

Runtime generation references:

- runtime manifest:
  [tools/console_router.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/console_router.py:350)
- sessions:
  [tools/console_router.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/console_router.py:388)

Input contract:

- for `process_stdio`, raw input bytes go directly to the wrapped process
- for `jsonl_frames`, input is encoded as `tx` JSONL frames
- for `binary_frames`, input is encoded as `tx` binary frames
- for `line_prefixes`, input bytes are forwarded unchanged to the wrapped
  process and only output classification is multiplexed

## Current X86 `vm_qemu_virtio` Implementation

### Producer-Side Framing

The x86 app now has native producer-side framing in the `Init` component.

References:

- VMM debug framing:
  [projects/vm/components/Init/src/main.c](/home/hlyytine/tii-sel4/projects/vm/components/Init/src/main.c:615)
- guest UART framing:
  [projects/vm/components/Init/src/serial.c](/home/hlyytine/tii-sel4/projects/vm/components/Init/src/serial.c:413)
- frame emission:
  [projects/vm/components/Init/src/console_frame_transport.c](/home/hlyytine/tii-sel4/projects/vm/components/Init/src/console_frame_transport.c:1)

Currently framed native sources:

- `driver_vm_console` as stream id `1`
- `vmm_debug` as stream id `7`

### Runner-Side Launch Contract

For x86 `binary_frames`, `qemu_runner.py` adjusts launch semantics so the
framed transport is not sharing the path with the QEMU monitor.

Important behavior:

- enables `-serial stdio -monitor none`
- emits a `binary_frames` manifest with
  `framing_mode = "producer_binary_native"`

Reference:

- [tools/qemu_runner.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/qemu_runner.py:620)

### Current Limitation

The x86 path is not yet fully frame-pure from first byte to last byte.

What is true today:

- the main VMM debug stream can be producer-owned and framed
- guest UART output can be producer-owned and framed
- the router supports framed transport and per-channel dispatch

What is still true today:

- some early or auxiliary bytes may still appear outside the framed contract
- the router therefore contains startup synchronization and resynchronization
  logic for `binary_frames`

Reference:

- [tools/console_router.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/console_router.py:233)

That resynchronization is a tolerance mechanism for incomplete producer
coverage. It is not a return to plaintext source heuristics.

## Architectural Rules

1. Producer-owned framing is the target authority model.
2. `binary_frames` is the preferred transport for split-console x86 work.
3. `line_prefixes` is legacy-only and must not be treated as a durable design.
4. Verdict-bearing automation must use named post-demux channels, not merged
   plaintext.
5. `tty0` may exist as compatibility surface or launch-failure surface, but it
   is not the source of truth for VM0 readiness once split routing is enabled.

## Related Documents

- [autopilot-console-source-integration.md](autopilot-console-source-integration.md)
- [autopilot-qemu-backends.md](autopilot-qemu-backends.md)
- [../integration/console-router-and-timeline-implementation-plan-2026-04-23.md](../integration/console-router-and-timeline-implementation-plan-2026-04-23.md)
