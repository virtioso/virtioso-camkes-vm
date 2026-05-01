# Console Stream Topology Diagrams

These diagrams are the visual companion to:

- [console-transport-and-routing.md](console-transport-and-routing.md)
- [autopilot-console-source-integration.md](autopilot-console-source-integration.md)

They are intended to answer four questions directly:

- where bytes are produced
- where muxing happens
- where demuxing happens
- what transport each edge uses

## Diagrams

- [console-stream-topology-binary-frames.d2](console-stream-topology-binary-frames.d2)
  - current x86 `qemu_x86_64` + `vm_qemu_virtio` target path
  - shows native producer framing, SSH carriage, router demux, and Autopilot
    interaction
- [console-stream-topology-line-prefixes.d2](console-stream-topology-line-prefixes.d2)
  - legacy compatibility path
  - shows where plaintext merging happens and where heuristic demux is applied
- [console-stream-topology-autopilot-io.d2](console-stream-topology-autopilot-io.d2)
  - consumer-side view
  - shows how Autopilot resolves sources, reads logs, and writes PTYs

## Reading Guide

- `binary_frames`:
  - producer owns stream identity
  - router decodes by `stream_id`
  - Autopilot consumes post-demux channels
- `line_prefixes`:
  - sources merge first
  - router guesses ownership from plaintext prefixes
  - fallback text lands in `driver_vm_console`

## Current Caveat

The `binary_frames` diagram reflects the current intended ownership and the
current live x86 producer split, but there is still an unresolved runtime bug:
remote source frames for stream `1` and `5` exist, yet the local router
runtime is not materializing them into the corresponding guest-console
`raw.log` files.
