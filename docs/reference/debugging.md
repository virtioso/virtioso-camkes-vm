# Debugging Guide (General)

This page provides a short, cross-platform debugging index.

## Common Debugging Entry Points

- **Kernel tracing**: `kernel/docs/ftrace.md` (from repo root)
- **Orin AGX**: `platforms/orin-agx/orin-agx-debugging-guide.md`
- **RAS / cache / PTW investigations**: `platforms/orin-agx/investigations/`

## Tips

- Start from the platform guide if you are on specific hardware.
- Use ftrace for scheduler and fault-path visibility.
- Keep test logs and request IDs with your notes.
