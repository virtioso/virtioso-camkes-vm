# Yocto QEMU Runtime Artifact Contract

## Summary

The x86 QEMU path currently depends on Yocto-native QEMU artifacts, but the
consumer boundary is wrong: `qemu_runner.py` reads directly from
`vm-images/build/tmp/work/...` and `sysroots-uninative` instead of consuming a
stable deploy artifact. The recommended design is for `meta-virtioso` to
produce a generic, relocatable host-QEMU runtime tarball, and for workspace
tooling to compose the final runnable bundle from that runtime plus the built
seL4 payload.

This keeps Yocto responsible for the generic host runtime and keeps app/workspace
layers responsible for app-specific `simulate` and `images/` outputs.

## Current State

Verified current producer/consumer split:

- `isengard` consumes Yocto guest deploy artifacts from
  `vm-images/build/tmp/deploy/images/qemux86-64/...` during the CAmkES/seL4
  build in projects/isengard-camkes-vm/apps/x86/isengard/CMakeLists.txt.
- The generic x86 QEMU bundle is assembled after `make <app>` in
  tools/qemu_runner.py.
- `qemu_runner.py` currently reaches into Yocto workdirs for:
  - `qemu-system-x86_64`
  - runtime libraries
  - QEMU data files
  - `pc-bios`
  - uninative loader
  via tools/qemu_runner.py and tools/qemu_runner.py.
- The seL4 side of the bundle is separate and already cleanly owned by the
  workspace build:
  - `simulate`
  - `images/`
  via tools/qemu_runner.py.
- Yocto `populate_sdk` provides a useful reference model for relocatable
  artifacts:
  - staged output tree and archived deploy output in
    populate_sdk_base.bbclass
    and populate_sdk_base.bbclass
  - installer-time relocation in
    toolchain-shar-extract.sh,
    toolchain-shar-relocate.sh,
    and relocate_sdk.py

Strong inference:

- The useful part to copy from `populate_sdk` is the explicit deploy artifact
  contract, not the interactive installer model.

## Recommended Contract

### Producer

Owner:

- `vm-images/virtioso-yocto-layers/meta-virtioso`

Artifact:

- `virtioso-qemu-runtime-x86_64.tar.zst`

Deploy path:

- `vm-images/build/tmp/deploy/virtioso-qemu-runtime/virtioso-qemu-runtime-x86_64.tar.zst`

Optional convenience symlink:

- `vm-images/build/tmp/deploy/virtioso-qemu-runtime/latest-x86_64.tar.zst`

Rationale:

- keeps the contract outside `tmp/work/...`
- allows one team member to produce the runtime once
- allows other team members to unpack and use it without a Yocto work tree

### Artifact Contents

Top-level layout inside the tarball:

```text
virtioso-qemu-runtime-x86_64/
  manifest.json
  README.runtime.md
  usr/
    bin/qemu-system-x86_64
    lib/
    lib64/
    libexec/
    share/qemu/
  pc-bios/
  uninative/
```

Required contents:

- `usr/bin/qemu-system-x86_64`
- runtime libraries needed by that binary
- `usr/share/qemu/`
- `pc-bios/`
- bundled uninative interpreter subtree if the binary requires it
- `manifest.json`

Excluded contents:

- app payload
- `simulate`
- seL4 `images/`
- Yocto workdir-only build byproducts such as:
  - dependency `.d` files
  - object `.o` files
  - build `Makefile`s
  - source-path descriptor files that are not needed at runtime

### Manifest

`manifest.json` should contain only artifact-local or semantic data.

Suggested schema:

```json
{
  "schema_version": 1,
  "artifact": "virtioso-qemu-runtime-x86_64",
  "host_arch": "x86_64",
  "qemu_binary": "usr/bin/qemu-system-x86_64",
  "qemu_version": "9.2.0",
  "launcher_model": "relative-runtime-tree",
  "includes_uninative": true,
  "entry_hint": "usr/bin/qemu-system-x86_64",
  "required_host_tools": ["bash", "tar", "ps", "setsid"],
  "required_host_capabilities": ["x86_64-linux", "kvm"],
  "build_id": "<yocto-build-id-or-pv-pr>"
}
```

Must not include:

- absolute build paths
- `${TOPDIR}`-derived paths
- developer home-directory paths

### Relocatability Contract

The artifact is relocatable by unpacking alone:

- no installer is required
- no path rewriting is required
- the consumer may unpack it anywhere
- launchers must use paths relative to the extracted tree

This differs deliberately from `populate_sdk`.

Reason:

- this artifact is a runtime package, not a host development SDK
- relative launchers are simpler than installer-time relocation
- it reduces host-side failure modes and onboarding friction

## Consumer Contract

### Workspace Bundle Composer

Owner:

- `projects/virtioso-camkes-vm/tools/qemu_runner.py`

New preferred input:

- `--runtime-dir /path/to/virtioso-qemu-runtime-x86_64`

Recommended resolution order for `qemu_runner.py`:

1. explicit `--runtime-dir`
2. extracted runtime artifact under a conventional workspace path
3. deployed runtime tarball under `vm-images/build/tmp/deploy/virtioso-qemu-runtime`
4. current fallback to Yocto `tmp/work/...`

The current workdir spelunking should remain only as a temporary compatibility
fallback.

### Final Runnable Bundle

The final bundle remains workspace-owned and app-specific:

- runtime artifact from `meta-virtioso`
- `simulate`
- seL4 `images/`
- bundle-local wrappers such as:
  - `runtime/run-bundle.sh`
  - `runtime/qemu-wrapper.sh`

This is already the model used by
tools/qemu_runner.py,
but the QEMU runtime producer boundary should be swapped to the new artifact.

### Local Intel Use Case

The same runtime artifact should also support a non-bundle local flow:

1. unpack `virtioso-qemu-runtime-x86_64.tar.zst`
2. use the unpacked runtime as the QEMU binary/data root
3. run local x86 QEMU-backed workflows from the workspace

This allows users who do not build Yocto locally to still use the x86 QEMU
backend if they have a compatible Intel/AMD Linux machine.

## Recommendations

### 1. Add a first-class runtime artifact in `meta-virtioso`

Rationale:

- makes the producer boundary explicit
- removes dependence on Yocto workdir internals
- enables team sharing

Affected areas:

- new recipe/class in `meta-virtioso`
- deploy output path under `tmp/deploy/virtioso-qemu-runtime`

Short-term benefit:

- one person can build the runtime artifact once
- other developers can reuse it without local Yocto builds

Migration implications:

- no change to app payload builds
- low risk if introduced alongside the current fallback path

### 2. Make `qemu_runner.py` consume the artifact instead of workdirs

Rationale:

- stabilizes the consumer side
- decouples the runner from Yocto internal directory structure

Affected areas:

- tools/qemu_runner.py
- tools/qemu_runner.py

Short-term benefit:

- cleaner sharing and CI handoff
- simpler support instructions

Migration implications:

- keep existing `_qemu_runtime()` workdir discovery temporarily as fallback

### 3. Keep final app bundle composition outside Yocto

Rationale:

- the final runnable bundle depends on workspace-generated `simulate` and
  `images/`
- those are not owned by BitBake today

Affected areas:

- workspace bundle scripts and `make` targets

Short-term benefit:

- avoids pulling the seL4 build into the Yocto task graph

Migration implications:

- no big-bang build-system inversion

## Validation Path

### Producer Validation

Verify the `meta-virtioso` artifact:

1. build the runtime artifact once from Yocto
2. unpack it under a non-workspace path
3. confirm:
   - no absolute `~/tii-sel4` references
   - `manifest.json` contains only portable metadata
   - bundled QEMU binary launches with relative paths

### Consumer Validation

Verify workspace composition:

1. `make mrproper`
2. `make qemu_x86_64_defconfig`
3. `make isengard`
4. compose the final bundle using the runtime artifact
5. extract the final bundle under a different prefix on another Intel host
6. run `./runtime/run-bundle.sh`

### Compatibility Validation

During migration, verify both paths:

- new runtime-artifact path
- old `tmp/work/...` fallback path

Acceptance:

- both produce equivalent runnable bundles
- fallback can be removed only after the new artifact is stable

## Risks And Open Questions

Verified facts:

- the current runtime bundle model is already generic
- the unstable part is the producer boundary, not the bundle shape

Strong inferences:

- `meta-virtioso` is the correct ownership layer for the generic runtime
- `populate_sdk` should influence the contract shape, but not be reused
  wholesale

Open questions:

- exact Yocto recipe/class shape for producing the runtime artifact
- whether `.tar.zst` or `.tar.gz` is preferred operationally
- whether the unpacked runtime should also ship a tiny helper launcher for
  local ad hoc use, or whether documentation is sufficient

Non-goal:

- this contract does not attempt to make app builds themselves independent of
  workspace-local source paths such as the hardcoded include roots in
  projects/isengard-camkes-vm/apps/x86/isengard/CMakeLists.txt.
