# Build and Test Runbook

This file is the canonical source for build and test command sequences.

## Orin AGX `vm_qemu_virtio`

1. Clean build state:
   `make mrproper`
2. Configure platform:
   `make orinagx_defconfig`
3. Build target:
   `make vm_qemu_virtio`
4. Verify binary exists:
   `/home/hlyytine/tii-sel4/orinagx_vm_qemu_virtio/images/capdl-loader-image-arm-orinagx`
5. Ensure Autopilot daemon is running in tmux:
   `mcp__sel4-autopilot__autopilot_restart(autopilot_dir="/home/hlyytine/tii-sel4/autopilot", use_tmux=true)`
6. Submit EFI test:
   `mcp__sel4-autopilot__test_sel4_efi(autopilot_dir="/home/hlyytine/tii-sel4/autopilot", binary_path="/home/hlyytine/tii-sel4/orinagx_vm_qemu_virtio/images/capdl-loader-image-arm-orinagx", profile="vm-qemu-virtio")`
7. Poll status:
   `mcp__sel4-autopilot__get_test_status(...)` or `mcp__sel4-autopilot__wait_for_test(...)`
8. Get logs:
   `mcp__sel4-autopilot__get_logs(...)`
9. If behavior is unexpected, inspect:
   `results/<id>/device-trees/`

## Orin AGX `vm_minimal`

1. `make mrproper`
2. `make orinagx_defconfig`
3. `make vm_minimal`
4. Verify binary:
   `/home/hlyytine/tii-sel4/orinagx_vm_minimal/images/capdl-loader-image-arm-orinagx`
5. Submit test with `test_sel4_efi` and profile `vm-minimal`.

## Orin AGX `sel4test`

1. `make mrproper`
2. `make orinagx_defconfig`
3. `make sel4test`
4. Verify binary:
   `/home/hlyytine/tii-sel4/orinagx_sel4test/images/sel4test-driver-image-arm-orinagx`
5. Submit test with `test_sel4_efi` and profile `sel4test`.

## Orin AGX `kmod-sel4-virt` (Yocto Module Recipe)

1. Configure platform (required by `vm-images/setup.sh`):
   `make orinagx_defconfig`
2. Ensure container image exists:
   `make docker`
3. Build module recipe with clean rebuild (default):
   `make kmod-sel4-virt`
4. Incremental rebuild only when explicitly requested:
   `YOCTO_INCREMENTAL=1 make kmod-sel4-virt`
5. Under the hood, this path runs:
   - `scripts/build_yocto_kmod_sel4_virt.sh`
   - `vm-images/setup.sh`
   - `bitbake -c cleansstate kernel-module-sel4-virt` (unless `YOCTO_INCREMENTAL=1`)
   - `bitbake kernel-module-sel4-virt`
6. Output location (Yocto work/deploy):
   `/home/hlyytine/tii-sel4/vm-images/build/tmp/work/*/kernel-module-sel4-virt/*`

## Notes

- Build commands are always `make` targets from workspace root.
- Testing is always `test_sel4_efi`.
- Profile mapping is `target.replace("_", "-")`.
- For daemon start/restart operations, always set `use_tmux=true`.
