# Prerequisites

This document describes the requirements for building and running the TII seL4 virtio virtualization platform.

## System Requirements

### Host Machine

| Requirement | Minimum | Recommended |
|-------------|---------|-------------|
| OS | Ubuntu 20.04+ | Ubuntu 22.04 |
| RAM | 16 GB | 32 GB |
| Disk | 100 GB | 200 GB |
| CPU | 4 cores | 8+ cores |

### Supported Build Hosts

- Ubuntu 20.04 LTS
- Ubuntu 22.04 LTS
- Debian 11 (Bullseye)

Other Linux distributions may work but are not officially supported.

## Required Software

### Git Configuration

```bash
git config --global user.email "you@example.com"
git config --global user.name "Your Name"
git config --global merge.ff only
```

### Docker

Docker is required for the containerized build environment:

```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install -y docker.io

# Add user to docker group
sudo usermod -aG docker $USER

# Log out and back in, then verify
docker --version
```

### Repo Tool

Google's repo tool manages the multi-repository workspace:

```bash
# Install repo
mkdir -p ~/.local/bin
curl https://storage.googleapis.com/git-repo-downloads/repo > ~/.local/bin/repo
chmod a+x ~/.local/bin/repo

# Add to PATH (add to ~/.bashrc)
export PATH="${HOME}/.local/bin:${PATH}"

# Verify
repo --version
```

## Environment Variables

### Required Variables

```bash
# Yocto source mirror (reduces download time)
export YOCTO_SOURCE_MIRROR_DIR=~/yocto-downloads
mkdir -p $YOCTO_SOURCE_MIRROR_DIR

# Build cache for Haskell packages
export BUILD_CACHE_DIR=~/.tii_sel4_build
mkdir -p ${BUILD_CACHE_DIR}/stack
```

Add these to your `~/.bashrc` for persistence.

### Optional Variables

```bash
# Custom workspace location
export WORKSPACE=~/sel4

# Container engine (default: docker)
export CONTAINER_ENGINE=docker  # or podman
```

## SSH Keys

SSH access is required for private repositories:

```bash
# Generate SSH key if needed
ssh-keygen -t ed25519 -C "your.email@example.com"

# Add to ssh-agent
eval "$(ssh-agent -s)"
ssh-add ~/.ssh/id_ed25519

# Add public key to GitHub
cat ~/.ssh/id_ed25519.pub
# Copy output and add to GitHub Settings → SSH Keys
```

## Hardware Requirements

### For QEMU Development

No special hardware required. QEMU ARM Virt platform is fully emulated.

### For Raspberry Pi 4

| Item | Notes |
|------|-------|
| Raspberry Pi 4 | 4GB or 8GB model |
| MicroSD Card | 32GB+ Class 10 |
| USB-UART Adapter | For serial console |
| Power Supply | Official 5V/3A PSU |
| Ethernet | For network boot (optional) |

## Disk Space Breakdown

| Component | Size |
|-----------|------|
| Source code | ~5 GB |
| Docker image | ~3 GB |
| Build cache | ~10 GB |
| Yocto downloads | ~20 GB |
| Build output | ~50 GB |

**Total**: ~90 GB minimum

## Network Requirements

The build process downloads:
- seL4 source repositories
- Yocto/OpenEmbedded layers
- Linux kernel source
- Toolchain packages
- Python packages

Ensure reliable internet connectivity during initial setup.

## Verification

After installing prerequisites, verify your setup:

```bash
# Check Git
git --version

# Check Docker
docker run hello-world

# Check repo
repo --version

# Check SSH access to GitHub
ssh -T git@github.com
```

## Troubleshooting

### Docker Permission Denied

```bash
# Ensure user is in docker group
groups | grep docker

# If not, add and re-login
sudo usermod -aG docker $USER
# Log out and back in
```

### Repo Init Fails

```bash
# Ensure SSH key is loaded
ssh-add -l

# Test GitHub access
ssh -T git@github.com
```

### Out of Disk Space

```bash
# Check available space
df -h

# Clean Docker artifacts
docker system prune -a

# Clean Yocto build artifacts
rm -rf vm-images/build/tmp
```

## Next Steps

Once prerequisites are installed:

1. [Initialize workspace](building.md#workspace-initialization)
2. [Build the project](building.md)
3. [Run on QEMU](running-qemu.md) or [RPi4](running-rpi4.md)
