# Autopilot QEMU Backend Architecture

This document defines the execution architecture for QEMU-backed targets used by
Virtioso seL4 builds and by autopilot.

Related console architecture documents:

- [virtual-channel-mux-architecture.md](virtual-channel-mux-architecture.md)
- [autopilot-console-source-integration.md](autopilot-console-source-integration.md)

## Goals

- Keep the deploy and boot flow backend-specific.
- Keep the final verdict logic backend-agnostic.
- Make the manual runner path the source of truth.
- Make autopilot consume the manual runner instead of duplicating QEMU launch logic.

## Backend Split

There are three execution backends:

1. `orinagx`
   - Deployment: copy EFI payload to the target over SSH.
   - Boot: Orin UEFI and relay/UART-backed workflow.
   - Console: hardware UARTs.
   - Post-run prepare: boot stock Linux again for the next round.

2. `qemu_arm64_defconfig`
   - Deployment: none beyond the local build tree.
   - Boot: local generated `simulate` script using the Yocto-built custom
     `qemu-system-aarch64`.
   - Console: process stdout/stderr.
   - Post-run prepare: stop QEMU when the run ends.

3. `qemu_x86_64_defconfig`
   - Deployment: package the generated `simulate` runtime tree plus the
     Yocto-built custom QEMU toolchain and copy it to an Intel host over SSH.
   - Boot: remote generated `simulate` script using the packaged
     `qemu-system-x86_64`.
   - Console: remote SSH session stdout/stderr.
   - Post-run prepare: stop remote QEMU / end the SSH-launched session.

These flows must not be forced into a common upload-and-boot sequence. The only
shared responsibility is result and verdict handling.

## Source Of Truth Boundary

The source of truth for QEMU-backed execution is the repo-owned manual runner:

- tools/qemu_runner.py

Autopilot may automate this runner, but it must not carry separate QEMU launch
knowledge that diverges from it.

The runner prefers the generated seL4 sibling `simulate` script because that is
the native seL4 manual entrypoint. It overrides the QEMU binary by passing
`-b <custom-qemu>` to the generated script. This keeps the command line aligned
with seL4 while still forcing the Yocto-built custom QEMU.

## Runtime Layout

### Local QEMU (`qemu_arm64_defconfig`)

- Build artifact path identifies the build directory.
- The runner uses `<build-dir>/simulate` when present.
- The runner prefers a deployed runtime artifact for x86_64 and otherwise
  resolves the Yocto-built QEMU toolchain under
  `vm-images/build/tmp/work/x86_64-linux/qemu-system-native/*`.
- It prefers the installed native path
  `recipe-sysroot-native/usr/bin/qemu-system-*`, but falls back to the Yocto
  recipe build tree `build/qemu-system-*` when the installed binary is absent.
- The runner sets `LD_LIBRARY_PATH` from `recipe-sysroot-native/usr/lib*`,
  exports `QEMU_DATA_DIR` from `recipe-sysroot-native/usr/share/qemu`, and
  passes `-L <.../pc-bios>` when the build-tree binary requires it.
- It then executes `./simulate -b <custom-qemu-system-aarch64>`.

### Remote QEMU (`qemu_x86_64_defconfig`)

- The runner creates a bundle with:
  - `runtime/build/simulate`
  - `runtime/build/images/*`
  - a selected QEMU runtime, preferably from the deployed artifact under
    `vm-images/build/tmp/deploy/virtioso-qemu-runtime/`
  - `toolchain/usr/bin/<selected-qemu-system-* binary>`
  - only the required shared libraries for that binary
  - `toolchain/usr/share/qemu*` when present
  - `toolchain/pc-bios/*` when present in the selected runtime
  - `runtime/run-bundle.sh`
- `runtime/run-bundle.sh` sets the bundled library path and runs:
  - `./simulate -b ../toolchain/usr/bin/qemu-system-x86_64`
- For `qemu_x86_64_defconfig`, the runner preserves the generated script's
  `-enable-kvm` requirement when merging custom extra QEMU arguments.
- The bundle is copied and executed over SSH on the configured Intel host.

Runtime source resolution order for the runner is:

1. `--runtime-dir`
2. `--runtime-tar`
3. deployed runtime artifact under `tmp/deploy/virtioso-qemu-runtime`
4. legacy Yocto `tmp/work/.../qemu-system-native` lookup

## Configuration Boundary

Machine-specific remote execution data lives in the user home directory:

- `~/.virtioso-qemu-runners.json`

Expected shape:

```json
{
  "runners": {
    "qemu_x86_64_defconfig": {
      "ssh_host": "intel-host.example",
      "ssh_user": "your-user",
      "remote_dir": "~/virtioso-qemu-runs/qemu_x86_64_defconfig",
      "ssh_options": ["-o", "StrictHostKeyChecking=no"]
    }
  }
}
```

The repo defines target meaning and launch semantics. The home config only
defines host-specific coordinates.

## Autopilot Integration Contract

Autopilot should treat QEMU-backed targets as process-backed sources rather than
UART-backed sources.

For the split-console x86 path, the detailed source-resolution and PTY-write
contract now lives in
[autopilot-console-source-integration.md](autopilot-console-source-integration.md).

Responsibilities that remain shared with Orin:

- request lifecycle
- result directory layout
- timeout and cancel handling
- pass/fail verdict extraction from console output

Responsibilities that stay backend-specific:

- artifact staging
- boot command construction
- local versus remote process ownership
- post-run teardown behavior

## Implementation Notes

- Prefer the generated `simulate` script whenever it exists.
- Use direct `qemu-system-aarch64` fallback only as a narrow backup for
  `qemu_arm64_defconfig`.
- Treat the deployed `meta-virtioso` runtime artifact as the preferred
  producer boundary for x86 host-QEMU runtime.
- Do not add stock-Linux recovery to QEMU flows.
- The manual runner must stay usable without autopilot.
