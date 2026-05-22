# Yocto QEMU Runtime Implementation Plan

## Summary

This plan implements the runtime-artifact contract in three slices:

1. `meta-virtioso` produces a generic relocatable x86 host-QEMU runtime tarball
2. `qemu_runner.py` consumes that artifact as the preferred runtime source
3. workspace docs and user workflows switch to the new producer/consumer path

The migration is coexistence-first. The current `tmp/work/...` discovery path
remains as a temporary fallback until the deployed artifact is proven stable.

## Scope

In scope:

- generic x86 host-QEMU runtime packaging
- workspace bundle composition changes
- local Intel usage and remote bundle composition workflows

Out of scope:

- changing app payload builds
- folding the full seL4 build into BitBake
- changing `isengard`-specific payload content

## Phase 1: `meta-virtioso` Producer

### Goal

Produce a first-class deploy artifact:

- `vm-images/build/tmp/deploy/virtioso-qemu-runtime/virtioso-qemu-runtime-x86_64.tar.zst`

### Ownership

- repo: `vm-images/virtioso-yocto-layers/meta-virtioso`
- likely implementation area:
  - `recipes-devtools/qemu/`
  - optional helper class under `classes/`

### Recommended file additions

- `recipes-devtools/qemu/virtioso-qemu-runtime-x86_64.bb`
- optional helper script under `files/`
- optional class if the pattern is expected to extend beyond x86_64

### Producer behavior

The recipe should stage a clean runtime tree containing:

- `usr/bin/qemu-system-x86_64`
- required native libs
- `usr/share/qemu`
- `pc-bios`
- uninative interpreter subtree if required
- `manifest.json`
- `README.runtime.md`

The recipe should archive that tree into the deploy artifact and optionally
write a `latest-x86_64.tar.zst` symlink.

### Producer implementation notes

- Do not copy from arbitrary workspace paths.
- Do not expose `tmp/work/...` as the public interface.
- It is acceptable for the recipe to gather from recipe-native sysroots and the
  native QEMU build output internally, but the exported artifact must be clean.
- Apply the same pruning already needed in the current bundle generator:
  - remove descriptor files carrying source paths
  - remove `.d`, `.o`, `Makefile`, and build-config byproducts
- Generate `manifest.json` from recipe variables rather than by copying runtime
  metadata files that may contain build paths.

### Acceptance criteria

- artifact exists under `tmp/deploy/virtioso-qemu-runtime/`
- unpacked tree contains no `~/tii-sel4` or `${TOPDIR}` references
- `usr/bin/qemu-system-x86_64` launches via a relative wrapper or directly with
  bundled libs

## Phase 2: `qemu_runner.py` Consumer

### Goal

Make the runner consume the deployed runtime artifact instead of rummaging in
Yocto workdirs.

### Ownership

- repo: `projects/virtioso-camkes-vm`
- file: tools/qemu_runner.py

### Consumer changes

Add a first-class runtime resolution path:

1. explicit `--runtime-dir`
2. explicit `--runtime-tar`
3. conventional deployed artifact under:
   - `vm-images/build/tmp/deploy/virtioso-qemu-runtime/virtioso-qemu-runtime-x86_64.tar.zst`
4. fallback to current Yocto `tmp/work/...` discovery

### Recommended CLI additions

- `--runtime-dir <path>`
- `--runtime-tar <path>`

These should apply to:

- `prepare-remote-bundle`
- `run-remote`
- optionally `run-local` for Intel users who want to reuse the runtime artifact

### Internal refactor

Split current runtime acquisition into two paths:

- artifact-backed runtime loading
- legacy Yocto workdir loading

Suggested helper seams:

- `_runtime_from_dir(path) -> QemuRuntime`
- `_runtime_from_tar(path, extract_root) -> QemuRuntime`
- `_runtime_from_deploy(target) -> QemuRuntime | None`
- keep `_qemu_runtime(spec)` only as legacy fallback

### Behavioral requirements

- composing a final bundle from an unpacked runtime artifact must produce the
  same `runtime/run-bundle.sh` behavior as today
- existing autopilot-backed x86 remote flow must keep working
- bundle metadata must remain portable and free of developer-home paths

### Acceptance criteria

- bundle generation works with no local Yocto workdir if runtime artifact is present
- old path still works during migration
- extracted final bundle still passes the existing path-leak grep check

## Phase 3: Docs And Workflow

### Goal

Make the new runtime artifact the documented happy path for teams.

### Ownership

- repo: `projects/virtioso-camkes-vm`
- docs likely touched:
  - `docs/start-here/running-qemu.md`
  - `docs/virtioso-camkes-vm/architecture/autopilot-qemu-backends.md`
  - `docs/virtioso-camkes-vm/agents/build-test-runbook.md`

### Required workflow changes

Document two supported flows.

#### Flow A: shared runtime, local Intel execution

1. one developer builds the Yocto runtime artifact
2. another developer unpacks it locally
3. developer builds `make qemu_x86_64_defconfig && make <app>`
4. local x86 QEMU execution uses the unpacked runtime artifact

#### Flow B: shared runtime, remote bundle composition

1. one developer builds the Yocto runtime artifact
2. another developer unpacks it or points the runner at the tarball
3. developer builds `make qemu_x86_64_defconfig && make <app>`
4. `qemu_runner.py prepare-remote-bundle` composes the final runnable bundle

### Instructions to add

- where the runtime artifact is produced
- how to unpack it
- how to pass it to the runner
- what host requirements remain:
  - x86_64 Linux
  - `/dev/kvm`
  - `bash`, `tar`, `ps`, `setsid`

### Acceptance criteria

- docs no longer describe `tmp/work/...` as the intended source of QEMU runtime
- developer instructions clearly separate:
  - generic runtime artifact
  - app-specific final bundle

## Validation Plan

### Slice 1 validation

Yocto producer only:

1. build the runtime artifact
2. unpack it under `/tmp`
3. verify:
   - expected tree layout
   - no absolute workspace-path references
   - QEMU binary is runnable with bundled libs

### Slice 2 validation

Runner consumer:

1. `make mrproper`
2. `make qemu_x86_64_defconfig`
3. `make isengard`
4. compose final bundle using:
   - `--runtime-dir`
   - and separately `--runtime-tar`
5. verify resulting bundle runs

### Slice 3 validation

Team workflow:

1. machine A builds Yocto runtime artifact once
2. machine B does not have the Yocto workdir
3. machine B builds `isengard`
4. machine B composes and runs the final bundle successfully

## Rollout Order

1. add `meta-virtioso` producer
2. verify artifact cleanliness and usefulness in isolation
3. add `qemu_runner.py` artifact consumption with fallback retained
4. switch docs to the new preferred path
5. after adoption, remove or de-emphasize direct `tmp/work/...` discovery

## Risks And Open Questions

Risks:

- hidden runtime dependencies may still be discovered only via the current
  Yocto workdir path
- QEMU firmware/BIOS selection may rely on files that appear incidental today
- `.tar.zst` may be operationally less convenient than `.tar.gz` for some users

Open questions:

- whether to produce both `.tar.zst` and `.tar.gz`
- whether the runtime artifact should ship a tiny `run-qemu.sh` helper for
  ad hoc manual use
- whether autopilot should later learn to consume the shared runtime artifact
  directly instead of relying only on final composed bundles

## Definition Of Done

The migration is complete when:

- the generic x86 host-QEMU runtime is produced in `meta-virtioso` as a clean
  deploy artifact
- `qemu_runner.py` prefers that artifact over Yocto workdirs
- developers can compose and run x86 QEMU bundles without performing local
  Yocto builds
- the documented happy path no longer depends on `tmp/work/...`
