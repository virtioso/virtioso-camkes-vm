# Preflight Policy

This file defines task-based preflight rules.

## Build/Test Requests

Read only:

1. [task-router.md](task-router.md)
2. [build-test-runbook.md](build-test-runbook.md)
3. [autopilot-testing-policy.md](autopilot-testing-policy.md)

## Example App Requests (`apps/Arm`)

Read only:

1. [task-router.md](task-router.md)
2. [example-workflows-fastpath.md](example-workflows-fastpath.md)

Do not read broad investigation docs for these requests unless explicitly asked.

## Yocto VM Image Requests (`vm-images/*`)

Read only:

1. [task-router.md](task-router.md)
2. [example-workflows-fastpath.md](example-workflows-fastpath.md#yocto-vm-image-fast-path)
3. [build-test-runbook.md](build-test-runbook.md) only if a build/test run is requested

Do path-scoped preflight only; do not restudy all of
`vm-images/virtioso-yocto-layers/` unless explicitly requested.

## Orin AGX Debug/Investigation Requests

Read:

1. [task-router.md](task-router.md)
2. [build-test-runbook.md](build-test-runbook.md)
3. [autopilot-testing-policy.md](autopilot-testing-policy.md)
4. `docs/platforms/orin-agx/orin-agx-debugging-guide.md`

### Extra Gate: Orin VM PCI INTx + Cross-VM IRQ Changes

For requests that modify any of the following on Orin VMs with PCI INTx usage:

- `vm*.dtb_irqs`
- platform free IRQ reserves (for example `free_plat_interrupts[]`)
- VM PCI INTx line usage/routing

do this preflight before edits:

1. Recompute currently used VM IRQs and free 8-bit-safe range (`32..255`).
2. Propose one or more replacement IRQ values with collision rationale.
3. Request explicit human affirmation of the chosen value before applying.

## Kernel Tracing and Ftrace Requests

Also read:

- `kernel/docs/ftrace.md`

Preflight gate before implementation work:

1. Apply tracing change-control policy from `AGENTS.md`.
2. Apply repo-level clean/branch checks from `docs/agents/repo-topology-policy.md`.
3. If any target repo is dirty, stop and request explicit human approval before
   continuing.

## Interactive Console Session Requests

Also read:

- `/home/hlyytine/autopilot/docs/ai-interactive-console.md`
