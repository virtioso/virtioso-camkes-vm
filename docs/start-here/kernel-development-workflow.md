# Kernel Development Workflow with Devtool

This document describes the recommended workflow for developing and testing changes to the guest Linux kernel in the Virtioso CAmkES VM platform.

## Overview

Yocto's `devtool` provides an efficient workflow for kernel development:

- **Incremental builds**: Only rebuild changed files, not the entire kernel
- **Git-based tracking**: Full history preserved, easy to create patches
- **Automatic patch generation**: No manual patch file editing required
- **Fast iteration**: Edit source → rebuild → test cycle

## Why Devtool Instead of Manual Patches

| Aspect | Manual Patches | Devtool Workflow |
|--------|---------------|------------------|
| Line numbers | Hardcoded, break when base changes | Auto-generated from git diff |
| Testing | Rebuild entire kernel each change | Incremental builds via externalsrc |
| Iteration | Edit patch file → rebuild → test | Edit source → rebuild → test |
| History | Lost unless manually tracked | Full git history preserved |
| Collaboration | Share patch files | Share git branches/commits |

## Prerequisites

- Docker container built (`make docker`)
- Initial Yocto build completed (`make linux-image`)
- Before any `bitbake` command, required local source repos are clean:
  - `~/tii-sel4/sources/qemu`
  - `~/tii-sel4/sources/kmod-sel4-virt`
  - `~/tii-sel4/sources/sel4-linux-kernel-support`
- For `sources/qemu`, initialize submodules:
  - `git -C ~/tii-sel4/sources/qemu submodule update --init --recursive`
- If any required repo is dirty, stop and decide with a human whether to commit or discard changes before continuing.

## Starting a Development Session

```bash
# Enter the vm-images directory and setup environment
cd ~/tii-sel4/vm-images && source setup.sh

# Start devtool session for the kernel
devtool modify linux-jammy-nvidia-tegra
```

This creates:
- **Source tree**: `build/workspace/sources/linux-jammy-nvidia-tegra/`
- **BBappend**: `build/workspace/appends/linux-jammy-nvidia-tegra_%.bbappend`

The workspace layer has priority 99, overriding all other layers.

## Making Changes

```bash
# Navigate to the kernel source
cd build/workspace/sources/linux-jammy-nvidia-tegra/

# Edit files directly
vim drivers/firmware/tegra/bpmp.c

# Stage and commit your changes
git add -A
git commit -m "descriptive commit message"
```

**Important**: Each git commit becomes a separate patch file when you finalize changes.

## Building and Testing

### Kernel-Only Changes

When you've only modified kernel source code (not initramfs contents, kernel modules, or init scripts):

```bash
# Build kernel - this also deploys Image to:
# vm-images/build/tmp/deploy/images/vm-jetson-agx-orin/Image
bitbake linux-jammy-nvidia-tegra

# Build the CAmkES VM application
# This uses images directly from Yocto deploy directory
cd ~/tii-sel4
make vm_qemu_virtio
```

### Changes Requiring Full Rebuild

When you've modified initramfs contents, kernel modules, or init scripts:

```bash
# Build kernel
bitbake linux-jammy-nvidia-tegra

# Build root filesystem image
bitbake vm-image-boot

# Copy images to CAmkES images directory (for Orin AGX)
cp ~/tii-sel4/vm-images/build/tmp/deploy/images/vm-jetson-agx-orin/vm-image-boot-vm-jetson-agx-orin.rootfs.cpio.gz \
   ~/tii-sel4/projects/camkes-vm-images/orinagx/rootfs.cpio.gz
cp ~/tii-sel4/vm-images/build/tmp/deploy/images/vm-jetson-agx-orin/Image \
   ~/tii-sel4/projects/camkes-vm-images/orinagx/linux

# Build the CAmkES VM application
cd ~/tii-sel4
make vm_qemu_virtio
```

### Testing on Hardware

Use the MCP autopilot tools for testing on Orin AGX. **Test tools are async**
and return a `request_id` immediately. Poll for completion.

```
# Submit (async)
resp = mcp__sel4-autopilot__test_sel4_efi(
    autopilot_dir="/home/hlyytine/tii-sel4/autopilot",
    binary_path="/home/hlyytine/tii-sel4/orinagx_vm_qemu_virtio/images/capdl-loader-image-arm-orinagx",
    profile="vm-qemu-virtio"
)
# resp contains request_id

# Poll status (recommended: 1s interval, 300s overall)
mcp__sel4-autopilot__get_test_status(request_id="...")
```

Notes:
- **Strict single-request policy**: a new submit fails if any request is pending or processing.
- Use `mcp__sel4-autopilot__autopilot_status` to see queue summary.
- Use `mcp__sel4-autopilot__cancel_test(request_id="...")` for hard cancel.

## Saving Changes to Recipe

When your changes are working and ready to be persisted:

### Preview Changes (Dry Run)

```bash
devtool update-recipe linux-jammy-nvidia-tegra --dry-run
```

### Generate Patches

```bash
devtool update-recipe linux-jammy-nvidia-tegra \
    -m patch \
    --append ~/tii-sel4/vm-images/virtioso-yocto-layers/meta-virtioso-sel4/dynamic-layers/tegra/recipes-kernel/linux
```

This converts git commits to patch files in:
```
vm-images/virtioso-yocto-layers/meta-virtioso-sel4/dynamic-layers/tegra/recipes-kernel/linux/linux-jammy-nvidia-tegra/
```

## Finishing a Session

### Option A: Keep Session Active (Recommended During Development)

Leave the session running for iterative development. Use `devtool update-recipe` periodically to sync patches.

### Option B: Finish and Close Session

```bash
# Ensure working tree is clean
cd build/workspace/sources/linux-jammy-nvidia-tegra/
git status  # Should show nothing to commit

# Finish session (moves patches to destination layer)
devtool finish linux-jammy-nvidia-tegra \
    ~/tii-sel4/vm-images/virtioso-yocto-layers/meta-virtioso-sel4/dynamic-layers/tegra/recipes-kernel/linux
```

### Option C: Reset (Discard Session)

```bash
devtool reset linux-jammy-nvidia-tegra
```

This moves the source to `build/workspace/attic/sources/` and removes the bbappend.

## Recovering an Archived Session

Previous sessions are archived in `build/workspace/attic/sources/`. To recover:

```bash
# List archived sessions
ls build/workspace/attic/sources/

# Copy archived source back to workspace
cp -r build/workspace/attic/sources/linux-jammy-nvidia-tegra.* \
      build/workspace/sources/linux-jammy-nvidia-tegra

# Use --no-extract to adopt existing source
devtool modify linux-jammy-nvidia-tegra --no-extract
```

## Working with NVIDIA Out-of-Tree Modules

The same workflow applies to NVIDIA kernel modules:

```bash
# Start session for nvidia-kernel-oot
devtool modify nvidia-kernel-oot

# Source is at:
# build/workspace/sources/nvidia-kernel-oot/

# Update recipe when done
devtool update-recipe nvidia-kernel-oot \
    -m patch \
    --append ~/tii-sel4/vm-images/virtioso-yocto-layers/meta-virtioso-sel4/dynamic-layers/tegra/recipes-kernel/nvidia-kernel-oot
```

## Directory Structure

```
vm-images/
├── build/
│   ├── workspace/
│   │   ├── sources/
│   │   │   └── linux-jammy-nvidia-tegra/  # Active workspace
│   │   ├── appends/
│   │   │   └── linux-jammy-nvidia-tegra_%.bbappend
│   │   └── attic/
│   │       └── sources/
│   │           └── linux-jammy-nvidia-tegra.<timestamp>/  # Archived
│   └── tmp/
│       └── deploy/
│           └── images/
│               └── vm-jetson-agx-orin/
│                   ├── Image  # Built kernel
│                   └── vm-image-boot-*.rootfs.cpio.gz  # Built initramfs
└── virtioso-yocto-layers/
    └── meta-virtioso-sel4/
        └── dynamic-layers/
            └── tegra/
                └── recipes-kernel/
                    └── linux/
                        └── linux-jammy-nvidia-tegra/
                            └── *.patch  # Persisted patches
```

## Troubleshooting

### Build Fails After Devtool Modify

Ensure you're in the correct environment:

```bash
cd ~/tii-sel4/vm-images && source setup.sh
```

### Changes Not Taking Effect

1. Verify the workspace is active:
   ```bash
   ls build/workspace/appends/
   ```

2. Clean the sstate cache if needed:
   ```bash
   bitbake -c cleansstate linux-jammy-nvidia-tegra
   bitbake linux-jammy-nvidia-tegra
   ```

### Merge Conflicts When Updating Base

If the upstream kernel changes and your patches no longer apply:

```bash
# Fetch upstream changes
cd build/workspace/sources/linux-jammy-nvidia-tegra/
git fetch origin

# Rebase your changes
git rebase origin/main  # or appropriate branch

# Resolve conflicts, then update recipe
devtool update-recipe linux-jammy-nvidia-tegra -m patch --append ...
```

## Best Practices

1. **Commit frequently**: Each commit becomes a logical patch
2. **Use descriptive commit messages**: They become patch descriptions
3. **Test incrementally**: Build and test after each significant change
4. **Update recipe before major base changes**: Preserve your work as patches
5. **Use dry-run first**: Preview what `update-recipe` will do before running it
