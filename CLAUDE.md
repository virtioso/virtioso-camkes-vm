# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## IMPORTANT: Read AGENTS.md First

Before using this repository, read `AGENTS.md` and follow its preflight requirements and defaults.

## CRITICAL: Autopilot Working Directory Configuration

**MANDATORY:** When calling ANY autopilot MCP tool, ALWAYS pass the `autopilot_dir` parameter set to `<working_directory>/autopilot`.

The working directory is shown in your environment info (e.g., `Working directory: /home/hlyytine/tii-sel4`).

**Example:** If working directory is `/home/hlyytine/tii-sel4`, use:
```
autopilot_dir="/home/hlyytine/tii-sel4/autopilot"
```

**Affected tools:** All `sel4-autopilot` MCP tools. See `AGENTS.md` for the current tool list and workflow.

This ensures autopilot data stays within the project directory and survives context compaction.

## CRITICAL: Orin AGX Debugging Documentation

**ALWAYS READ BEFORE ANY ORIN AGX WORK:**

📚 **Master Guide**: `projects/virtioso-camkes-vm/docs/platforms/orin-agx/orin-agx-debugging-guide.md`

This guide links to all investigation documents and contains:
- Current bug status (what's fixed, what's in progress)
- Proven facts and disproven hypotheses (don't re-investigate!)
- Memory layout, build configs, testing procedures

**After compaction/context loss**: Re-read the master guide before continuing work.

**Key documents** (via master guide):
| Document | Purpose |
|----------|---------|
| `orin-ras-error-investigation.md` | 167KB investigation log - READ SUMMARY FIRST |
| `sel4-memory-layout-orinagx.md` | Physical memory layout, kernel load addresses |
| `tegra-cache-operations.md` | CRITICAL: dc civac vs dc cisw |
| `kernel/docs/ftrace.md` | Function tracing for kernel debugging |
| `vm-image-boot-unification-plan.md` | IN PROGRESS: Unify vm-image-boot with minimal-init |

## Project Overview

TII seL4 Virtio Virtualization Platform - runs multiple isolated guest VMs on the seL4 microkernel (ARM architecture) with virtio device virtualization. QEMU runs inside a "device VM" to provide virtio backends to "driver VMs". Uses CAmkES component framework.

**Supported Platforms:** Raspberry Pi 4, QEMU ARM Virt

## Build Commands

All builds run inside a Docker container. **IMPORTANT: Always run `make` commands from the workspace root (`~/tii-sel4/`), NOT from `virtioso-build/` subdirectory.** The Makefile is symlinked to the workspace root, and paths in build scripts assume execution from there.

```bash
# Build Docker container (first time setup)
make docker

# Configure for platform
make raspberrypi4-64_defconfig    # Raspberry Pi 4
make qemuarm64_defconfig          # QEMU ARM Virt

# Build guest Linux images (Yocto - takes 2-4 hours first time)
make linux-image

# Build CAmkES VM applications
make vm_qemu_virtio               # 2 VMs: device + driver
make vm_virtio_multi_user         # 3 VMs: device + 2 drivers
make vm_minimal                   # Basic VM test
make sel4test                     # seL4 test suite

# Run QEMU simulation
make simulate_vm_qemu_virtio

# Enter container shell interactively
make shell

# Pre-populate Haskell build cache
make build_cache
```

### Incremental Builds

```bash
# Rebuild CAmkES app after source changes
make vm_qemu_virtio

# Rebuild specific Yocto image
make shell
cd vm-images && source setup.sh
bitbake vm-image-driver

# Clean CAmkES build
rm -rf rpi4_vm_qemu_virtio

# Clean Yocto (preserves downloads)
rm -rf vm-images/build/tmp
```

## Repository Structure

| Directory | Description |
|-----------|-------------|
| `kernel/` | seL4 microkernel (TII fork with modifications) |
| `projects/virtioso-camkes-vm/` | Main TII project - virtio VMM components |
| `projects/sel4_projects_libs/` | VMM libraries (libsel4vm, libsel4vmmplatsupport) |
| `projects/vm/` | CAmkES VM framework |
| `projects/vm-linux/` | Guest Linux integration, camkes-connector modules |
| `projects/camkes-tool/` | CAmkES component framework |
| `sources/kmod-sel4-virt/` | Kernel module bridging QEMU ↔ seL4 RPC (/dev/sel4) |
| `sources/qemu-sel4-virtio/` | QEMU fork with seL4 accelerator (accel/sel4) |
| `virtioso-build/` | Build system, Docker, scripts, configs |
| `vm-images/meta-sel4/` | Yocto layer for guest images |
| `tools/seL4/` | seL4 build tools (cmake-tool, elfloader) |

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                      seL4 Microkernel                        │
├──────────────────────────────────────────────────────────────┤
│  Device VM (Linux)          │  Driver VM (Linux)             │
│  ┌────────────────────┐     │  ┌────────────────────┐       │
│  │ QEMU + seL4 accel  │     │  │ virtio drivers     │       │
│  │ vso_rpc library    │     │  │ User applications  │       │
│  │ kmod-sel4-virt     │     │  │ SWIOTLB bounce buf │       │
│  └─────────┬──────────┘     │  └─────────┬──────────┘       │
│            │                │            │                   │
│            └─────── RPC via shared dataport ─────────        │
│                    (iobuf + memdev)                          │
└──────────────────────────────────────────────────────────────┘
```

**Key concepts:**
- VMs are peers (not host/guest hierarchy) - CAmkES defines connections
- I/O Proxy in VMM intercepts MMIO faults, forwards via RPC to QEMU
- SWIOTLB provides cross-VM DMA bounce buffer
- GICv2m emulation enables MSI for vhost acceleration

## Key Source Files (projects/virtioso-camkes-vm/)

| File | Purpose |
|------|---------|
| `src/libsel4vm_glue.c` | Core VMM integration, RPC message handling |
| `src/io_proxy.c` | I/O request/response lifecycle, fault handling |
| `src/fdt.c` | Device tree generation for guest VMs |
| `src/msi.c` | MSI/GICv2m interrupt handling |
| `configurations/virtioso/vm.h` | CAmkES component macros (VIRTIO_COMPOSITION_DEF) |
| `templates/*.template.c` | CAmkES component templates for virtio VMs |
| `apps/Arm/vm_qemu_virtio/` | Main 2-VM demo application |
| `include/sel4/rpc.h` | RPC opcode definitions |

## CAmkES Application Structure

```
apps/Arm/vm_qemu_virtio/
├── CMakeLists.txt           # App build config
├── vm_qemu_virtio.camkes    # Component assembly
├── rpi4/
│   └── devices.camkes       # Platform-specific device config
└── qemu-arm-virt/
    └── devices.camkes
```

CAmkES files (`.camkes`) define VM components and connections:
- `VIRTIO_COMPOSITION_DEF(device_vm_id, driver_vm_id)` creates virtio connection
- `vm_virtio_devices` attribute: VM provides virtio backends
- `vm_virtio_drivers` attribute: VM uses virtio devices

## RPC Protocol

Driver VM ↔ Device VM communication uses shared memory RPC queues:

| Opcode | Direction | Purpose |
|--------|-----------|---------|
| `QEMU_OP_MMIO` (0) | Driver→Device | MMIO read/write |
| `QEMU_OP_SET_IRQ` (16) | Device→Driver | Inject interrupt |
| `QEMU_OP_START_VM` (18) | Device→Driver | QEMU ready signal |
| `QEMU_OP_REGISTER_PCI_DEV` (19) | Device→Driver | Register PCI device |

## Debugging

```bash
# QEMU with GDB
qemu-system-aarch64 ... -s -S
aarch64-linux-gnu-gdb kernel/build/kernel.elf -ex "target remote :1234"

# Trace config (raspberrypi4-64_trace_defconfig)
make raspberrypi4-64_trace_defconfig

# Container shell for manual builds
make shell
```

## Documentation

Detailed documentation in `projects/virtioso-camkes-vm/docs/`:
- `architecture/` - System overview, virtio architecture, RPC protocol
- `components/` - I/O Proxy, PCI passthrough, interrupt handling
- `start-here/` - Build and run instructions
- `integration/` - QEMU backend, kernel modifications, kmod-sel4-virt
- `platforms/orin-agx/reference/tegra-cache-operations.md` - **CRITICAL: Tegra dc civac vs dc cisw issue**
- `platforms/orin-agx/reference/tegra-whole-cache-operations.md` - **ARM64 whole-cache operations (dc cisw broken on ALL ARM64)**
- `platforms/orin-agx/investigations/orin-ras-error-investigation.md` - RAS error investigation on Orin AGX

## Tegra Platform Notes (Xavier/Orin)

**CRITICAL: Cache flush operations on Tegra Xavier and Orin require `dc civac` instead of `dc cisw`.**

The `dc cisw` (clean and invalidate by set/way) instruction does not work correctly on these platforms - it invalidates cache lines without properly writing data to DRAM first, causing data corruption.

**Always use `dc civac` (clean and invalidate by VA to PoC) for cache maintenance on Tegra.**

See `projects/virtioso-camkes-vm/docs/platforms/orin-agx/reference/tegra-cache-operations.md` for details, code examples, and affected components that need auditing.

**OPEN BUG: RAS Errors on Orin AGX**

⚠️ **MUST READ**: See master guide at `projects/virtioso-camkes-vm/docs/platforms/orin-agx/orin-agx-debugging-guide.md`

**Status (2025-12-20):**
- **Bug B (0x0xxx errors)**: ✓ FIXED - safe PTEs during unmap
- **Bug A (0x7fffxxxx errors)**: ⚠️ IN PROGRESS - PTE overwrite mystery

**CRITICAL: Cache Flush AND ATF/OP-TEE Hypotheses DISPROVEN**

- Non-temporal stores (STNP) that bypass cache: did NOT fix the problem (Phase 16)
- 5-second delay before verification: verification PASSES, later scan finds garbage (Phase 17)

This proves:
1. Cache flush operations (dc civac) ARE working correctly
2. Safe PTEs ARE being written to DRAM
3. ATF/OP-TEE is NOT corrupting memory
4. **Corruption happens DURING test execution** - something in the test or kernel writes to PT memory

**Key Facts:**
- RAS errors are NOT random hardware noise - triggered by specific code paths
- Only 7/122 sel4test tests trigger errors; 115 tests NEVER trigger errors
- **Primary triggers**: FPU0001 (76%), CANCEL_BADGED_SENDS_0002 (32%)
- Page tables are initialized correctly but get corrupted with stale data AFTER creation
- Garbage PTE values point to addresses from PREVIOUS test iterations

**Where to look for the bug:**
- Something that writes to PT memory AFTER Arch_createObject()
- Memory reuse/retype paths during cap revocation
- Possible DMA or other bus master activity

**Where NOT to look:**
- Cache flush operations (dc civac works correctly)
- Arch_createObject() initialization (verified correct)
- Basic IPC, scheduling, CNode ops, memory retyping

**Stress test for reproducing**: Run 100× CANCEL_BADGED_SENDS_0002

## Orin AGX Development Resources

### Source Code Locations
- **Linux kernel sources**: `~/tii-sel4/vm-images/build/workspace/sources/linux-jammy-nvidia-tegra`
- **NVIDIA out-of-tree modules** (nvidia-oot, nvdisplay, nvgpu, hwpm, nvethernetrm): `~/tii-sel4/vm-images/build/workspace/sources/nvidia-kernel-oot`
- **OP-TEE and ATF sources**: `~/pkvm/Linux_for_Tegra/source/tegra/optee-src`
- **UEFI sources**: `~/nvidia-uefi-docker/nvidia-uefi`

### Live System Access
- SSH to running Linux: `ssh root@192.168.101.112`
- If SSH does not respond, ask the user to boot the board to Linux

## MANDATORY: Guest Kernel Development Workflow

**CRITICAL:** Always use `devtool modify` for kernel development. Do NOT manually edit patch files.

### Starting Development

```bash
cd ~/tii-sel4/vm-images && source setup.sh
devtool modify linux-jammy-nvidia-tegra
```

### Making Changes

```bash
cd build/workspace/sources/linux-jammy-nvidia-tegra/
# Edit files, then commit
git add <files>
git commit -m "descriptive message"
```

### Building (Fast Path)

```bash
# For kernel-only changes:
bitbake linux-jammy-nvidia-tegra  # Builds and deploys Image
cd ~/tii-sel4 && make vm_qemu_virtio  # Uses Image from deploy dir

# For initramfs/module changes, add:
bitbake vm-image-boot
# Then copy to camkes-vm-images (see vm_minimal workflow)
```

### Saving Changes to Recipe

```bash
devtool update-recipe linux-jammy-nvidia-tegra \
    -m patch \
    --append ~/tii-sel4/vm-images/virtioso-yocto-layers/meta-virtioso-sel4/dynamic-layers/tegra/recipes-kernel/linux
```

### Recovering Archived Sessions

```bash
# List archived sessions
ls build/workspace/attic/sources/

# Recover
cp -r build/workspace/attic/sources/linux-jammy-nvidia-tegra.* \
      build/workspace/sources/linux-jammy-nvidia-tegra
devtool modify linux-jammy-nvidia-tegra --no-extract
```

See: `projects/virtioso-camkes-vm/docs/start-here/kernel-development-workflow.md`

## Repo Roots & Symlinked Paths

This workspace is a repo manifest checkout, not a single git repository. Many
paths at `~/tii-sel4` are linkfiles/symlinks into other repos.

Before `git add` or `git commit`, always work from the real repo root:

```bash
git -C <path> rev-parse --show-toplevel
```

If you are editing `docs/` (linked to this repo), commit from:
`projects/virtioso-camkes-vm/`.

## Orin AGX Testing with Autopilot

**IMPORTANT:** Always use the autopilot framework to run seL4 binaries on Orin AGX. The autopilot handles UEFI navigation, log capture, and board recovery automatically.

### Using the Autopilot (Recommended)

```python
# From Python (e.g., in Claude Code)
import sys
sys.path.insert(0, '/home/hlyytine/autopilot')
from sel4_client import submit_sel4_test, wait_for_result, get_sel4_log

# Submit test and wait for results
timestamp = submit_sel4_test(
    binary_path='/home/hlyytine/tii-sel4/orinagx_sel4test/images/sel4test-driver-image-arm-orinagx',
    binary_name='sel4test.efi'
)
result = wait_for_result(timestamp)
log = get_sel4_log(timestamp)
print(log)
```

```bash
# From command line
cd /home/hlyytine/autopilot
./sel4_client.py submit ~/tii-sel4/orinagx_sel4test/images/sel4test-driver-image-arm-orinagx --name sel4test.efi --wait
```

### Autopilot Features
- Automatic UEFI menu navigation to EFI shell
- Captures output until 30 seconds quiescent
- Filters bootloader/UEFI output via profile-defined `analyze_logs` steps
- Automatic recovery to stock Linux after each test
- Skips unnecessary reboots when board is already ready
 
### Results Location
- Profile-defined logs live under: `/home/hlyytine/tii-sel4/autopilot/results/<timestamp>/console/`

### UART Reliability

**IMPORTANT: The UART connection does NOT drop bits or bytes.** Any data corruption issues in ftrace/binary transfers are due to encoding bugs, not UART unreliability. The autopilot's UART capture is reliable.

If you encounter LZ4 decompression failures in ftrace data, the issue is in the encoder, not UART corruption. The LZ4 encoder in `kernel/src/benchmark/lz4.c` must respect the decoder's safety constraints:
- No matches within 12 bytes of uncompressed end (MFLIMIT check)
- Last 5 bytes must be literals (LASTLITERALS check)

### Build/Test Workflow (Authoritative)

Build/test workflows, tool selection, and log locations are governed by `AGENTS.md`.
This section intentionally defers to `AGENTS.md` to avoid drift or conflicts.

## External Dependencies

- seL4 microkernel: https://sel4.systems/
- CAmkES: Component architecture for seL4
- Yocto: Guest Linux image builds
- QEMU: Device emulation (TII fork with seL4 accelerator)
