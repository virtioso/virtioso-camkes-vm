# Building

This document provides step-by-step instructions for building the TII seL4 virtio virtualization platform.

## Workspace Initialization

### Clone Repositories

```bash
# Create workspace directory
export WORKSPACE=~/sel4
mkdir -p $WORKSPACE && cd $WORKSPACE

# Initialize repo with TII manifest
repo init -u git@github.com:tiiuae/tii_sel4_manifest.git -b tii/development

# Sync all repositories
repo sync -j$(nproc)
```

This downloads approximately 5GB of source code.

### Directory Structure

After sync, the workspace contains:

```
$WORKSPACE/
├── kernel/                 # seL4 microkernel
├── projects/
│   ├── tii-sel4-vm/       # Main TII project
│   ├── sel4_projects_libs/ # VMM libraries
│   ├── vm/                # CAmkES VM framework
│   ├── vm-linux/          # Guest Linux integration
│   └── ...
├── tools/
│   └── seL4/              # seL4 tools
├── tii_sel4_build/        # Build system
├── vm-images/
│   └── meta-sel4/         # Yocto layer
├── Makefile               # Symlink to build system
└── ...
```

## Build Docker Container

Build the containerized build environment:

```bash
cd $WORKSPACE

# Build Docker image
make docker
```

This creates `tiiuae/build:latest` with all build dependencies.

## Configure Platform

### Raspberry Pi 4

```bash
make raspberrypi4-64_defconfig
```

### QEMU ARM Virt

```bash
make qemuarm64_defconfig
```

### Configuration Options

View/edit configuration:

```bash
# View current config
cat .config

# Manual editing
vim .config
```

## Build Guest Images

Build Yocto-based guest Linux images:

```bash
# Build all guest images
make linux-image
```

This builds:
- `vm-image-driver` - Device VM image with QEMU
- `vm-image-user` - Driver VM image
- `vm-image-boot` - Boot/initramfs

**Note**: First build takes 2-4 hours depending on hardware and network.

### Build Output

Images are placed in:
```
$WORKSPACE/vm-images/build/tmp/deploy/images/<machine>/
├── vm-image-driver-<machine>.ext4.qcow2
├── vm-image-user-<machine>.ext4.qcow2
└── vm-image-boot-<machine>.cpio.gz
```

## Build CAmkES Application

### vm_qemu_virtio (2 VMs)

```bash
make vm_qemu_virtio
```

### vm_virtio_multi_user (3 VMs)

```bash
make vm_virtio_multi_user
```

### Other Targets

```bash
# Basic VM test
make vm_minimal

# Multi-VM without virtio
make vm_multi

# seL4 test suite
make sel4test
```

## Build Output

CAmkES build output:

```
$WORKSPACE/rpi4_vm_qemu_virtio/
├── images/
│   ├── capdl-loader-image-arm-bcm2711  # Boot image
│   └── ...
└── ...
```

## Full Build Example

Complete build from scratch:

```bash
# 1. Setup workspace
export WORKSPACE=~/sel4
mkdir -p $WORKSPACE && cd $WORKSPACE

# 2. Initialize and sync
repo init -u git@github.com:tiiuae/tii_sel4_manifest.git -b tii/development
repo sync -j$(nproc)

# 3. Build Docker
make docker

# 4. Configure for RPi4
make raspberrypi4-64_defconfig

# 5. Build guest images
make linux-image

# 6. Build CAmkES app
make vm_qemu_virtio
```

## Incremental Builds

### Rebuild CAmkES Only

```bash
# After source changes
make vm_qemu_virtio
```

### Rebuild Guest Images

```bash
# Rebuild specific image
cd vm-images
source setup.sh
bitbake vm-image-driver
```

### Clean Build

```bash
# Clean CAmkES build
rm -rf rpi4_vm_qemu_virtio

# Clean Yocto build (preserves downloads)
rm -rf vm-images/build/tmp

# Full clean
make clean
```

## Build Cache

### Haskell Cache

The build uses cached Haskell packages:

```bash
# Pre-populate cache (saves time on rebuild)
make build_cache
```

Cache location: `~/.tii_sel4_build/stack/`

### Yocto Shared State

Yocto uses shared state cache for incremental builds.

Location: `vm-images/build/sstate-cache/`

## Troubleshooting

### Docker Build Fails

```bash
# Check Docker is running
sudo systemctl status docker

# Rebuild without cache
docker build --no-cache -t tiiuae/build docker/
```

### Yocto Build Fails

```bash
# Check for common issues
cd vm-images
source setup.sh
bitbake -e vm-image-driver | grep ^DISTRO

# Clean and retry
bitbake -c cleanall vm-image-driver
bitbake vm-image-driver
```

### Out of Memory

For systems with limited RAM:

```bash
# Limit parallel jobs
PARALLEL_MAKE="-j2" make vm_qemu_virtio
```

### Network Issues

```bash
# Use local source mirror
export YOCTO_SOURCE_MIRROR_DIR=/path/to/mirror
```

## Build Times

Approximate build times on 8-core system:

| Component | First Build | Incremental |
|-----------|-------------|-------------|
| Docker image | 5 min | N/A |
| Guest images | 2-4 hours | 5-30 min |
| CAmkES app | 15-30 min | 2-5 min |

## Next Steps

After successful build:

1. [Run on QEMU](running-qemu.md)
2. [Run on Raspberry Pi 4](running-rpi4.md)
