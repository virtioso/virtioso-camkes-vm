# X86 `vm_qemu_virtio` Framed Console Transport Plan

Date: 2026-04-24

Status: historical, superseded

Superseded by:

- [../architecture/cross-arch-tcu-uart-mux-plan-2026-05-01.md](../architecture/cross-arch-tcu-uart-mux-plan-2026-05-01.md)

This note is retained as evidence from the April 2026 x86 investigation. It is
not a live transport plan. `line_prefixes`, `binary_frames`, `jsonl_frames` as
a mux transport, and `CF` framing are local-only post-upstream work and may be
rewritten or deleted. The live target is the forked TCU-style `0xfe` escape
protocol in the superseding architecture note.

Tracker: [Console Router And Timeline Implementation Plan](console-router-and-timeline-implementation-plan-2026-04-23.md)

Historical architecture references:

- [../architecture/console-transport-and-routing.md](../architecture/console-transport-and-routing.md)
- [../architecture/autopilot-console-source-integration.md](../architecture/autopilot-console-source-integration.md)

This note remains a dated x86 migration record, not the primary architecture
document.

## Summary

The current `line_prefixes` transport is not acceptable as the target
architecture for x86 `vm_qemu_virtio`. It performs post-hoc source inference on
already-merged plaintext output, which makes channel identity heuristic,
newline-sensitive, and vulnerable to formatting drift.

The old replacement target was a producer-owned framed transport with no
heuristics:

- the producer emits explicit channel records
- the router only decodes and dispatches those records
- Autopilot matches only against per-channel buffers after demux
- merged human-readable logs become derived artifacts, not authoritative inputs

## Current Problem

Verified from code:

- `tools/qemu_runner.py` selects `transport.type = "line_prefixes"` for
  `vm_qemu_virtio` when `VIRTIOSO_CONSOLE_ROUTER_USE_VM_PREFIX_DEMUX=1`
  (qemu_runner.py).
- `tools/console_router.py` then classifies the merged byte stream using
  textual prefixes such as `[vmmdbg] `, `vm0: `, and `vm1: `
  (console_router.py).
- unmatched text falls through to `driver_vm_console`
  (qemu_runner.py).

Operationally, that means:

- channel identity is guessed after merging
- guest prompts depend on fallback behavior
- match quality depends on prefixes, line breaks, and ANSI normalization
- the router is not a dispatcher; it is a classifier

That ownership model is wrong for prompt detection, VM attribution, and
timeline construction.

## Hard Requirements

The replacement transport must satisfy all of these:

1. No source heuristics in the router.
2. No prefix matching for channel identity.
3. No fallback channel for unframed bytes.
4. No line-oriented attribution.
5. No verdict-bearing regex execution on merged streams.
6. Input writes must target explicit logical channels.
7. Human-readable merged logs may exist, but only as derived views.

## Target Architecture

## Current Implementation Status

The code has now moved beyond the temporary wrapper-only stage:

- `tools/console_router.py` supports `transport.type = "binary_frames"`
- `tools/qemu_runner.py` emits a `binary_frames` manifest for
  `vm_qemu_virtio` when `VIRTIOSO_CONSOLE_ROUTER_USE_BINARY_FRAMES=1`
- the x86 `vm_qemu_virtio` app now compiles its `Init` components with
  `-DVMM_CONSOLE_FRAMED_OUTPUT=1`
- `projects/vm/components/Init/src/main.c` routes VMM stdio through a framed
  debug sink
- `projects/vm/components/Init/src/serial.c` routes guest UART bytes through a
  framed `driver_vm_console` sink
- `tools/qemu_runner.py` forces `-serial stdio -monitor none` in
  `binary_frames` mode so the QEMU monitor no longer shares the framed
  transport

What that proves:

- the muxer/demuxer path can now run with no router-side heuristics
- the x86 producer can emit at least two native stream ids directly:
  `driver_vm_console` and `vmm_debug`
- the clean x86 build still succeeds with the new transport enabled

What remains unresolved:

- the producer split is now materially better, but not yet sufficient for
  usable guest-console automation
- after the 2026-04-25 stream-ownership changes:
  - generic `vm0:` / `vm1:` diagnostic text lands on stream `3`
    (`vmm_mux_control`)
  - explicit heartbeats land on stream `7` (`vmm_debug`)
  - the remote producer emits real guest-console frames for stream `1`
    (`driver_vm_console`) and stream `5` (`user_vm_console`)
- however, in the latest live run
  `/tmp/qemu-x86-binary-frames-thinkpad-fix3`, the local router runtime still
  leaves `driver_vm_console/raw.log` and `user_vm_console/raw.log` empty even
  though the remote source frames exist
- the remaining blocker is therefore no longer “can the producer emit the
  correct stream ids?”; it is “why does the local `binary_frames` runtime not
  materialize valid guest-console frames for interactive channels?”

So the architecture after the current slice is:

- no heuristics in the router
- native producer framing exists for generic VMM diagnostics, explicit debug,
  and instance-aware guest UART sources
- the overall x86 path is not yet usable end to end because the local router
  still fails to surface valid guest-console stream `1` / `5` payload in the
  interactive channel logs

### Producer Contract

The producer must emit framed records over a single transport stream. Each
record carries:

- `stream_id`
- `direction`
- `payload_length`
- `payload`

Recommended binary frame shape:

- `magic`: 2 bytes
- `version`: 1 byte
- `stream_id`: 1 byte
- `direction`: 1 byte
- `flags`: 1 byte
- `payload_length`: 4 bytes, big-endian
- `payload`: `payload_length` bytes

Direction values:

- `0x01`: producer -> router (`rx`)
- `0x02`: router -> producer (`tx`)

This keeps the authority boundary simple:

- the producer owns stream identity
- the router owns dispatch and persistence
- consumers own interpretation after dispatch

### Why Not Escape-Based Sentinel Framing

A sentinel scheme such as:

- `NUL` starts frame control
- doubled `NUL` means literal `NUL`
- bytes up to next `NUL` select stream

would be far better than `line_prefixes`, but it is still an escape protocol
over a raw stream. The main drawbacks are:

- more parser state
- harder corruption recovery
- harder resynchronization after dropped bytes
- less obvious artifact inspection

So the architectural recommendation is:

- accept the spirit of the idea: explicit inline framing
- reject escape-heavy sentinel framing as the final wire contract
- use length-delimited binary frames instead

## Proposed Stream Map

Use stable numeric stream ids:

- `0x01` `driver_vm_console`
- `0x02` `driver_vm_control`
- `0x03` `vmm_mux_control`
- `0x04` `nested_qemu_control`
- `0x05` `user_vm_console`
- `0x06` `trace_control`
- `0x07` `vmm_debug`

The names already exist in the manifest/channel set defined by
qemu_runner.py.
The missing part is not taxonomy; it is producer-side authority.

## Router Contract

The router should become a pure frame dispatcher:

1. read frame
2. validate header and length
3. resolve `stream_id`
4. append raw payload to that stream's log
5. emit event metadata
6. forward `rx` payload to PTY if the stream is interactive

The router must not:

- inspect plaintext prefixes to choose a channel
- normalize ANSI to choose a channel
- infer fallback ownership for unclassified bytes

If an unknown `stream_id` appears, the router should fail the run loudly rather
than guessing.

## Autopilot Contract

Autopilot should consume only named channels after demux.

That means:

- each channel maintains its own append-only raw log
- each channel has its own rolling matcher buffer
- only matchers for that channel rerun when new bytes arrive

Example:

- new bytes on `driver_vm_console` rerun login and shell matchers
- new bytes on `vmm_debug` do not affect VM0 readiness
- `tty0` may still exist as a rendered compatibility artifact, but it must not
  own the pass/fail semantics for split-console runs

## Migration Plan

### Slice 1: Declare the Target and Freeze Semantics

Touch:

- this document
- tracker document
- stream inventory note

Goal:

- make `line_prefixes` explicitly legacy-only
- make framed transport the only acceptable target state

Validation:

- documentation and chain references are internally consistent

### Slice 2: Add `binary_frames` Transport To `console_router.py`

Touch:

- `tools/console_router.py`

Goal:

- add a new transport mode that decodes binary framed records
- keep existing per-channel PTY/event/log behavior

Validation:

- local synthetic transport smoke test
- mixed-channel payloads route without plaintext inspection

### Slice 3: Add Producer-Side Framed Emission

Touch:

- `tools/qemu_runner.py`
- remote wrapper / producer path inside the x86 run bundle

Goal:

- replace merged plaintext ownership with explicit stream emission

Validation:

- each logical channel receives only its own payload
- no fallback classification path is exercised

### Slice 4: Move X86 Login Chains To Framed Sources Only

Touch:

- Autopilot x86 QEMU-backed chains

Goal:

- `driver_vm_console` becomes the only authority for VM0 login readiness
- `vmm_debug` becomes diagnostics-only
- `tty0` becomes launch-compatibility only

Validation:

- `qemu_x86_64_vm_qemu_virtio_login_probe`
- `qemu_x86_64_vm_qemu_virtio_minimal_login`
- `qemu_x86_64_vm_qemu_virtio_uservm`

### Slice 5: Remove `line_prefixes` From X86 Verdict Paths

Touch:

- x86 transport selection in `qemu_runner.py`
- x86 Autopilot chain assumptions

Goal:

- no x86 verdict-bearing path depends on heuristic demux

Validation:

- a login prompt fragment without trailing newline still appears correctly on
  `driver_vm_console`
- `vmm_debug` traffic never influences login matching

## Validation Path

Build and test path to validate the migration:

1. `make mrproper`
2. `make qemu_x86_64_defconfig`
3. `make vm_qemu_virtio`
4. run:
   - `qemu_x86_64_vm_qemu_virtio_login_probe`
   - `qemu_x86_64_vm_qemu_virtio_minimal_login`
   - `qemu_x86_64_vm_qemu_virtio_uservm`

Evidence of success:

- `driver_vm_console` receives prompt bytes directly
- `root` injection is recorded as `tx` on `driver_vm_console`
- `vmm_debug` receives only explicit debug frames
- no router code path depends on prefix maps or fallback channels

## Risks And Open Questions

- The exact producer insertion point for framed emission still needs code-level
  confirmation inside the remote x86 runner path.
- `jsonl_frames` already exists as a declared transport mode, but it is still a
  text framing layer; it may be useful as an intermediate seam, but it should
  not be confused with the final no-heuristics binary transport.
- If some existing producer can only emit legacy merged stdout today, the
  migration may need a temporary adapter. That adapter should be treated as a
  transitional producer shim, not as a permanent router responsibility.
