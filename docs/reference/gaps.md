# Documentation Gaps and Fixups

This file tracks documentation gaps found during a review of `projects/tii-sel4-vm/docs`.

## Broken Links

- `getting-started/running-qemu.md` links to `../reference/debugging.md` which does not exist.
- `getting-started/running-rpi4.md` links to `../reference/debugging.md` which does not exist.

## Clarity Gaps

- `index.md` lists only QEMU ARM Virt and Raspberry Pi 4 as supported platforms, but there is extensive Orin AGX porting and investigation material in `docs/porting` and `docs/reference`. Consider explicitly labeling Orin AGX as in-progress/porting in the index.

## Freshness Gaps

- Several Orin AGX investigation documents include status dates around 2025-12-20 to 2025-12-31. As of 2026-02-04, these are over a month old and may benefit from a “last reviewed” note or a quick status refresh.
