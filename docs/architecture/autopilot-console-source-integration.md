# Autopilot Console Source Integration

This document describes how Autopilot consumes the repo-owned console router
runtime and how chain sources map onto router-managed channels.

The transport and muxer/demuxer architecture itself is defined in
[console-transport-and-routing.md](console-transport-and-routing.md).

## Summary

Autopilot does not implement its own console demuxer for the x86 split-console
path. It consumes the router runtime produced by
`projects/virtioso-camkes-vm/tools/console_router.py`.

The integration boundary is:

1. `qemu_runner.py` emits `console-manifest.json`
2. `console_router.py` turns that into `console-runtime/`
3. Autopilot resolves chain `source` names against `sessions.json` and channel
   `legacy_aliases`
4. `wait_pattern` reads per-channel logs, and `send_cmd` writes to the
   interactive channel PTY

## Repo-Owned Artifacts

For QEMU-backed split-console runs, the repo-owned launch path writes these
artifacts under the result `console/` directory:

- `console-manifest.json`
- `console-runtime/runtime-manifest.json`
- `console-runtime/sessions.json`
- `console-runtime/channels/<name>/raw.log`
- `console-runtime/channels/<name>/events.jsonl`
- `console-runtime/channels/<name>/pty` for interactive channels

Autopilot uses these artifacts rather than reverse-engineering channel layout
itself.

## Source Resolution In Autopilot

Autopilot source resolution is implemented in
[/home/hlyytine/autopilot/chain_runtime.py](/home/hlyytine/autopilot/chain_runtime.py:453).

Mechanics:

- Autopilot loads `console/console-manifest.json`
- Autopilot loads `console/console-runtime/sessions.json`
- it joins channels by channel name
- it additionally exposes each channel through any declared `legacy_aliases`

That means a channel such as `driver_vm_console` can also be visible to chains
as `tty0` when the manifest declares that alias.

This is a consumer-side alias map only. It does not change transport authority.

## Read Path

Autopilot reads router-managed channel logs by source name.

Reference:

- [read_router_since in `chain_runtime.py`](/home/hlyytine/autopilot/chain_runtime.py:485)

Behavior:

- source name resolves to a router session
- session `log_path` points at the channel `raw.log`
- `wait_pattern` and related steps read only that source's log

Architectural consequence:

- once a chain uses `source: "driver_vm_console"`, its regexes run on the
  demuxed VM0 console stream rather than on merged launcher text

## Write Path

Autopilot `send_cmd` targets explicit sources.

Reference:

- [\_step_send_cmd in `chain_runtime.py`](/home/hlyytine/autopilot/chain_runtime.py:1014)

Behavior:

- if the source is a normal bound source, Autopilot writes via that binding
- otherwise it resolves the router session PTY for the source
- it opens that PTY and writes the command payload there

In router-backed split-console runs, this means:

- `send_cmd` on `driver_vm_console` writes to the PTY owned by that channel
- the router emits a `tx` event for that channel
- the router forwards the bytes to the wrapped process according to the active
  transport

Transport-specific consequences:

- `line_prefixes`: raw input goes straight to process stdin
- `binary_frames`: router wraps the payload in a `tx` binary frame carrying the
  target channel id

## Current X86 Chain Pattern

The active x86 chains already use split sources in practice.

Example:

- [qemu_x86_64_vm_qemu_virtio_login_probe.json](/home/hlyytine/autopilot/chains/qemu_x86_64_vm_qemu_virtio_login_probe.json:1)

Current shape:

- `tty0` is still watched for early runner failure such as
  `QEMU_RUNNER_ERROR:`
- `driver_vm_console` is watched for:
  - `driver-vm login:`
  - root shell prompt
  - post-login probe behavior
- `send_cmd` writes `root\n` to `driver_vm_console`

This is the correct split of responsibilities:

- compatibility and launch-failure coverage on `tty0`
- verdict-bearing login automation on `driver_vm_console`

## Interaction With Legacy Aliases

Legacy aliases exist to let old source names keep working during migration.

Example:

- `driver_vm_console` currently declares `legacy_aliases: ["tty0"]`

This is useful for:

- older chains
- early compatibility
- result inspection tools that still assume `tty0`

It is dangerous when:

- a chain treats `tty0` as the authority for a split-console verdict
- the underlying transport is still polluted or compatibility-only

Rule:

- use the named channel as the verdict source
- use aliases only for migration or compatibility

## Current Limitations

1. Most Autopilot chains still assume `tty0` as the primary source because the
   wider system predates the split-console x86 work.
2. The x86 split-console chains still carry some `tty0` logic for launch
   failures and compatibility.
3. If the manifest exposes a compatibility alias, careless chain authors can
   accidentally regress back to merged-source verdicts.

## Architectural Rules

1. Autopilot must consume router-managed named channels, not implement its own
   demux rules.
2. `source` in a chain is a post-demux source selector.
3. `send_cmd` to a router-managed source must target that channel's PTY, not a
   guessed global stdin path.
4. Legacy aliases are a migration tool, not a source-of-truth mechanism.
5. Verdict-bearing x86 VM0 login chains should prefer `driver_vm_console` over
   `tty0`.

## Related Documents

- [console-transport-and-routing.md](console-transport-and-routing.md)
- [autopilot-qemu-backends.md](autopilot-qemu-backends.md)
- [../integration/console-router-and-timeline-implementation-plan-2026-04-23.md](../integration/console-router-and-timeline-implementation-plan-2026-04-23.md)
