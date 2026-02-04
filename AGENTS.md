# AGENTS.md

This file provides guidance to Codex (and other coding agents) for working in this repository.

## Mandatory Preflight (Before Any Planning or Implementation)

Always read the following documents before you start planning or making changes:

1. `CLAUDE.md`
2. `docs/README.md`
3. `docs/index.md`

If the task involves Orin AGX or platform-specific debugging, also read:

4. `docs/platforms/orin-agx/orin-agx-debugging-guide.md`

If the task involves kernel tracing, ftrace, or scheduler/IRQ behavior, also read:

5. `../../kernel/docs/ftrace.md`

If the task involves build, Yocto, or CI/CD, also read:

6. `docs/build-system/build-architecture.md`
7. `docs/build-system/yocto-integration.md`
8. `docs/build-system/ci-cd.md`

## Additional Notes

- Treat these preflight docs as required context. If they are missing or outdated, note that explicitly before proceeding.
- When unsure, prefer the docs under `docs/` as the source of truth for this repository.
