# Example Workflows Fast Path

This file defines startup shortcuts for example-project and Yocto VM-image work.

## Goal

Reduce new-session startup time by routing immediately to the relevant subtree
without broad repo or layer re-reading.

## Example Projects Fast Path

When the request targets example projects, start with path-scoped context only.
Treat any direct child directory name under each `apps/Arm` root as a valid
project-name token for intent routing (for example `vm_minimal`,
`vm_virtio_multi_user`, and other siblings).

### Scope A: `projects/virtioso-camkes-vm/apps/Arm`

- Treat this as virtio-camkes-vm example application work.
- First read only files directly related to the named example target.
- Expand scope only when the requested change crosses target boundaries.

### Scope B: `projects/vm-examples/apps/Arm`

- Treat this as vm-examples application work.
- First read only files directly related to the named example target.
- Expand scope only when the requested change crosses target boundaries.

## Yocto VM Image Fast Path

When the request mentions Yocto-built VM images, route directly to `vm-images/*`
and avoid full-layer restudy.

### Primary entrypoints

- `vm-images/virtioso-yocto-layers/meta-virtioso/images/vm-image-boot.bb`
- `vm-images/virtioso-yocto-layers/meta-virtioso/recipes-devtools/qemu/qemu-rnd-helper.bb`
- `vm-images/virtioso-yocto-layers/meta-virtioso/recipes-devtools/qemu/qemu-rnd-helper/`
- `vm-images/virtioso-yocto-layers/meta-virtioso-sel4/recipes-core/images/vm-image-boot.bbappend`

### Scope discipline

- Prefer recipe-local and source-local reads first.
- Read other layers or classes only when dependency tracing requires it.
- Do not treat build outputs (`vm-images/build/*`) as source of truth for edits.

## DRY and SSOT Rules

- Keep command authority in `build-test-runbook.md`.
- Keep routing authority in `task-router.md`.
- Keep preflight authority in `preflight-policy.md`.
- Use this file only for fast-scope startup and path mapping.
