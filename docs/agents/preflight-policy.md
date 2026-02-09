# Preflight Policy

This file defines task-based preflight rules.

## Build/Test Requests

Read only:

1. [task-router.md](task-router.md)
2. [build-test-runbook.md](build-test-runbook.md)
3. [autopilot-testing-policy.md](autopilot-testing-policy.md)

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

## Interactive Console Session Requests

Also read:

- `/home/hlyytine/autopilot/docs/ai-interactive-console.md`
