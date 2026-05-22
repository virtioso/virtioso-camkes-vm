# Build and Test Runbook

This file is the canonical source for build and test command sequences.

## Isengard Linux Orin — Chain Reference

There are three chains for Isengard Orin AGX bare-metal Linux. Pick based on
the goal:

| Chain | Use when | Ends with |
|---|---|---|
| `isengard-linux-orin` | CI / login-proof boot check | recovery boot |
| `isengard-linux-orin-demo` | Verify end-to-end pipeline (vcan0 → isengard-demo-start → FUSE /obj) | recovery boot |
| `isengard-linux-orin-interactive` | Leave device in Isengard rootfs for human console/SSH session | recovery boot after human closes console |

### Rules for AI agents

- **Never SSH to `root@192.168.101.112` without first knowing the device is in
  the Isengard rootfs.** After a `pass` from `isengard-linux-orin` or
  `isengard-linux-orin-demo`, the device has already recovered to stock Linux.
- To SSH into a running Isengard rootfs, submit `isengard-linux-orin-interactive`
  and poll until `last_step.step == "interactive"`. At that point the device is
  at the Isengard login prompt and SSH works. **Do not poll waiting for the
  chain to complete** — it blocks until a human closes the console session.
- After the human closes the interactive console, the chain completes and
  recovery boot runs automatically.
- `isengard-linux-orin-demo` is the right choice for AI-only end-to-end
  verification. It SSHes in, runs `isengard-demo-start`, reads the FUSE object
  tree, and returns pass/fail with captured output in the log.

### Build step (Isengard Linux image)

```
make isengard-linux-image
```

Both `isengard-initramfs` (kernel+initramfs EFI) and `isengard-rootfs` (ext4)
are built by this single target. The chain uploads both automatically.

### Submit — CI boot check

```
autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot submit efi \
  --chain isengard-linux-orin \
  --binary /home/hlyytine/tii-sel4/vm-images/build/tmp/deploy/images/isengard-agx-orin/Image-initramfs-isengard-agx-orin.bin \
  --json
```

### Submit — End-to-end demo (AI-safe)

```
autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot submit efi \
  --chain isengard-linux-orin-demo \
  --binary /home/hlyytine/tii-sel4/vm-images/build/tmp/deploy/images/isengard-agx-orin/Image-initramfs-isengard-agx-orin.bin \
  --json
```

After pass, inspect logs to confirm `sequence`, `status_word`, and `uptime_ms`
were read from `/obj/isengard/`:
```
autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot logs <request-id> \
  --file raw_ccplex.txt --grep "sequence\|obj tree\|vcan0" --include-contents --json
```

### Submit — Interactive (human-supervised)

```
autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot submit efi \
  --chain isengard-linux-orin-interactive \
  --binary /home/hlyytine/tii-sel4/vm-images/build/tmp/deploy/images/isengard-agx-orin/Image-initramfs-isengard-agx-orin.bin \
  --json
```

Poll until device is at interactive hold:
```
autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot get <request-id> --json
# wait for last_step.step == "interactive"
```

While `last_step.step == "interactive"`, SSH is live:
```
ssh root@192.168.101.112
isengard-demo-start
isengard-demo-watch
```

The human closes the autopilot console session to release the chain and trigger
recovery boot.

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
   `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot restart --platform orin-agx-uefi-netboot --tmux --tty0 /dev/ttyACM0 --tty1 /dev/ttyACM1 --json`
6. Submit EFI test:
   `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot submit efi --chain vm-qemu-virtio --binary /home/hlyytine/tii-sel4/orinagx_vm_qemu_virtio/images/capdl-loader-image-arm-orinagx --json`
7. Poll status:
   `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot status --json` or `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot get <request-id> --json`
8. Get logs:
   `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot logs <request-id> --json`
9. For VM-specific proof, inspect the demuxed guest console sink first. For VM1
   `uname -a`, use:
   `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot logs <request-id> --file console-runtime/virtioso_mux_logs/vm1_guest_console_sink.txt --grep "Linux user-vm" --include-contents --json`
10. Do not treat raw host captures such as `tty1.raw` as the primary VM1 proof
    when mux/demux logs exist; they may contain firmware or carrier-console
    traffic instead of the logical VM stream.
11. If behavior is unexpected, inspect:
   `results/<id>/device-trees/`

## `qemu_x86_64_defconfig` `vm_qemu_virtio`

1. Clean build state:
   `make mrproper`
2. Configure platform:
   `make qemu_x86_64_defconfig`
3. Build target:
   `make vm_qemu_virtio`
4. Verify binary exists:
   `/home/hlyytine/tii-sel4/qemu_x86_64_vm_qemu_virtio/images/capdl-loader-image-x86_64-pc99`
5. Ensure Autopilot daemon is running in tmux:
   `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot restart --platform qemu-generic --tmux --json`
6. Submit QEMU-backed test:
   `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot submit efi --chain qemu_x86_64_defconfig --binary /home/hlyytine/tii-sel4/qemu_x86_64_vm_qemu_virtio/images/capdl-loader-image-x86_64-pc99 --build-platform qemu_x86_64 --json`
7. Poll status:
   `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot status --json` or `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot get <request-id> --json`
8. Get logs:
   `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot logs <request-id> --json`
9. Use direct `tools/qemu_runner.py` only for backend debugging when the autopilot integration itself is suspect.
10. For direct runner debugging, prefer a deployed or unpacked runtime artifact
    over the legacy `tmp/work/.../qemu-system-native` path.

## `qemu_x86_64_defconfig` `vm_qemu_virtio` With VM1 Launch Validation

1. Clean build state:
   `make mrproper`
2. Configure platform:
   `make qemu_x86_64_defconfig`
3. Build target:
   `make vm_qemu_virtio`
4. Verify binary exists:
   `/home/hlyytine/tii-sel4/qemu_x86_64_vm_qemu_virtio/images/capdl-loader-image-x86_64-pc99`
5. Ensure Autopilot daemon is running in tmux:
   `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot restart --platform qemu-generic --tmux --json`
6. Submit the deeper VM1-launch validation:
   `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot submit efi --chain qemu_x86_64_vm_qemu_virtio_uservm --binary /home/hlyytine/tii-sel4/qemu_x86_64_vm_qemu_virtio/images/capdl-loader-image-x86_64-pc99 --build-platform qemu_x86_64 --json`
7. Poll status:
   `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot status --json` or `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot get <request-id> --json`
8. Get logs:
   `autopilot --autopilot-dir /home/hlyytine/tii-sel4/autopilot logs <request-id> --json`
9. Inspect VM-specific managed-launch evidence in the demuxed guest console sink
   first, for example:
   `results/<id>/console/console-runtime/virtioso_mux_logs/vm1_guest_console_sink.txt`
10. The `qemu_x86_64_defconfig` chain remains the boot-to-login smoke test;
    `qemu_x86_64_vm_qemu_virtio_uservm` is the x86 profile that runs
    `uservmctl start`, `uservmctl wait-ready`, VM1 root login on
    `user_vm_console`, and a VM1 `uname -a`.

## Orin AGX `vm_minimal`

1. `make mrproper`
2. `make orinagx_defconfig`
3. `make vm_minimal`
4. Verify binary:
   `/home/hlyytine/tii-sel4/orinagx_vm_minimal/images/capdl-loader-image-arm-orinagx`
5. Submit test with `autopilot submit efi --chain vm-minimal --binary /home/hlyytine/tii-sel4/orinagx_vm_minimal/images/capdl-loader-image-arm-orinagx --json`.

## Orin AGX `sel4test`

1. `make mrproper`
2. `make orinagx_defconfig`
3. `make sel4test`
4. Verify binary:
   `/home/hlyytine/tii-sel4/orinagx_sel4test/images/sel4test-driver-image-arm-orinagx`
5. Submit test with `autopilot submit efi --chain sel4test --binary /home/hlyytine/tii-sel4/orinagx_sel4test/images/sel4test-driver-image-arm-orinagx --json`.

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

## x86_64 QEMU Runtime Artifact

1. Configure platform (required by `vm-images/setup.sh`):
   `make qemu_x86_64_defconfig`
2. Ensure container image exists:
   `make docker`
3. Build the relocatable host runtime artifact:
   `make qemu-runtime-x86_64`
4. Under the hood, this path runs:
   - `scripts/build_yocto_qemu_runtime_x86_64.sh`
   - `vm-images/setup.sh`
   - `bitbake virtioso-qemu-runtime-x86_64`
5. Output location:
   `/home/hlyytine/tii-sel4/vm-images/build/tmp/deploy/virtioso-qemu-runtime/virtioso-qemu-runtime-x86_64.tar.zst`

## Notes

- Build commands are always `make` targets from workspace root.
- Defconfig commands are serialized steps. Do not run `make <name>_defconfig`
  in parallel with any other command.
- Testing is always through the `autopilot` command-line API.
- Use Autopilot chains, not legacy profiles.
- For QEMU defconfig targets, the Autopilot chain name matches the build
  target name exactly.
- For daemon start/restart operations, always use `--tmux`.
- Do not mix Autopilot APIs. Do not use MCP tools, direct queue files, or Python
  internals as fallback paths.
- `qemu_x86_64_defconfig` uses the autopilot remote-QEMU backend with chain
  `qemu_x86_64_defconfig`.
- For manual x86 QEMU runs, prefer the deployed runtime artifact under
  `vm-images/build/tmp/deploy/virtioso-qemu-runtime/` or pass an explicit
  `--runtime-dir` / `--runtime-tar` to `tools/qemu_runner.py`.
