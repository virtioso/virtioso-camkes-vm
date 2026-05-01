# Agent Task Router

This file is the canonical task router for operational agent workflows.

## Build and Test

| User intent | Use this runbook |
|---|---|
| Build and test `vm_qemu_virtio` on Orin AGX | [build-test-runbook.md](build-test-runbook.md#orin-agx-vm_qemu_virtio) |
| Build and test `vm_minimal` on Orin AGX | [build-test-runbook.md](build-test-runbook.md#orin-agx-vm_minimal) |
| Build and test `sel4test` on Orin AGX | [build-test-runbook.md](build-test-runbook.md#orin-agx-sel4test) |
| Build `kmod-sel4-virt` via Yocto on Orin AGX | [build-test-runbook.md](build-test-runbook.md#orin-agx-kmod-sel4-virt-yocto-module-recipe) |

## Fast Scope Routing

| User intent | Use this source |
|---|---|
| Ask whether we were working on a topic, recover lost context, or find an old plan/investigation | [context-index.md](context-index.md) |
| Work on `projects/virtioso-camkes-vm/apps/Arm` examples | [example-workflows-fastpath.md](example-workflows-fastpath.md) |
| Work on `projects/vm-examples/apps/Arm` examples | [example-workflows-fastpath.md](example-workflows-fastpath.md) |
| Work on Yocto-built VM image content under `vm-images/*` | [example-workflows-fastpath.md](example-workflows-fastpath.md#yocto-vm-image-fast-path) |

## Policy

| Topic | Canonical doc |
|---|---|
| Preflight rules | [preflight-policy.md](preflight-policy.md) |
| Autopilot testing policy | [autopilot-testing-policy.md](autopilot-testing-policy.md) |
| Repo topology and commit roots | [repo-topology-policy.md](repo-topology-policy.md) |
