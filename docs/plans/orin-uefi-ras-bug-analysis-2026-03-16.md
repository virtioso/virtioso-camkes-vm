# Orin UEFI RAS Bug Analysis

Date: 2026-03-16
Status: Root-caused, not yet fixed
Scope: NVIDIA UEFI firmware on Orin AGX

## Summary

After heavy UEFI shell usage (e.g., `ls` on a directory with many files),
launching a seL4 EFI application triggers a UEFI-internal data abort followed
by RAS errors. The fault originates inside UEFI, not in seL4 or the
elfloader.

## Evidence

RAS error captured during a manual test launch after extended UEFI shell use:

```
ELR_EL3 (seL4 PC when interrupted): 0x8273bfa20
SPSR_EL3: 0x800003c8 (EL=2)
ELR_EL2: 0x8273bedd4
ESR_EL2: 0x9600000b
FAR_EL2: 0x8273c52a0
TTBR0_EL2: 0x82cbdc000
VTTBR_EL2: 0x0
```

## Address Resolution

Module load addresses from UEFI shell `dh -v` (saved in `~/tii-sel4/dh.txt`):

| Address | Module | Function | Notes |
|---------|--------|----------|-------|
| ELR_EL3 `0x8273bfa20` | ArmCpuDxe (base `0x8273B8000`) | `SynchronousExceptionSPx` | UEFI exception vector entry |
| ELR_EL2 `0x8273bedd4` | ArmCpuDxe (base `0x8273B8000`) | `CommonCExceptionHandler` (ArmExceptionLib.c:275) | UEFI C exception handler |
| FAR_EL2 `0x8273c52a0` | **Gap** between ArmCpuDxe (ends `0x8273C3000`) and next module (`0x8273C7000`) | n/a | Faulting VA is in unallocated heap between DXE modules |

The faulting address (`FAR_EL2`) is in a **16KB gap between loaded DXE
modules** — this is UEFI heap memory that is either freed or unallocated.

## Root Cause

1. UEFI's `ls` command on a large directory (`fs2:\bootefi` with many files)
   causes heavy heap allocation in the filesystem/shell drivers
2. This likely causes heap fragmentation or a buffer overflow/use-after-free
3. A stale pointer survives into the subsequent EFI application launch
4. When UEFI follows this stale pointer, it accesses `0x8273c52a0` which is
   in a gap between loaded modules (ArmCpuDxe and the next DXE driver)
5. This causes a **synchronous data abort** (ESR_EL2 = `0x9600000b`)
6. UEFI's exception handler (ArmCpuDxe `SynchronousExceptionSPx` →
   `CommonCExceptionHandler`) begins handling the abort
7. While inside the exception handler, ATF delivers the **RAS error**
   asynchronously
8. ATF captures the exception handler's PC as ELR_EL3

The RAS error is a secondary symptom. The primary bug is the UEFI data abort.

## Memory Map Context

From the EFI memory map dump:

```
[13] 0x826512000 - 0x8273f0000  BootSvcCode  (3806 pages, ~15MB)
```

All UEFI DXE drivers are loaded in this region. The faulting addresses are
near the end of this region. Total UEFI `.text` from build output is ~15MB,
matching this region exactly.

ATF prints `Entry point address = 0x82c800000` — this is the UEFI/BL33
entry. The elfloader is loaded by UEFI at `0x819a81000` (LoaderCode region),
far from the faulting addresses.

## UEFI Source References

- ArmCpuDxe: `edk2/ArmPkg/Drivers/CpuDxe/`
- Exception handler: `edk2/ArmPkg/Library/ArmExceptionLib/ArmExceptionLib.c:275`
- Exception vectors: `edk2/ArmPkg/Library/ArmExceptionLib/AArch64/ExceptionSupport.S`
- Shell: `edk2/ShellPkg/Application/Shell/`
- Fat filesystem: `edk2/FatPkg/` (if fs2: is FAT)
- UEFI build: `~/nvidia-uefi-docker/nvidia-uefi/`
- Debug symbols: `~/nvidia-uefi-docker/nvidia-uefi/Build/Jetson/DEBUG_GCC5/AARCH64/`
- Module handle dump: `~/tii-sel4/dh.txt`

## Workaround Options

### Option 1: Pre-launch TLBI in elfloader (before ExitBootServices)

Add a TLBI before calling `efi_exit_boot_services()`. At that point UEFI's
page tables are still fully valid (boot services haven't been reclaimed),
so TLBI + re-walk is safe. This flushes any stale TLB entries accumulated
during the shell session.

This won't fix the UEFI heap corruption bug itself, but may prevent the
stale TLB entries from causing the data abort. However, if the bug is a
stale DATA pointer (not a stale TLB entry), TLBI won't help.

### Option 2: Boot seL4 via L4TLauncher/extlinux instead of EFI shell

L4TLauncher is NVIDIA's standard boot path for Linux. It loads kernel images
via UEFI's `LoadImage`/`StartImage` services without going through the shell.
This avoids the shell's heap-heavy filesystem operations entirely.

To use L4TLauncher for seL4:
- The seL4 EFI binary would need to be placed where extlinux.conf points
- L4TLauncher expects a Linux-style kernel + DTB + optional initrd
- The seL4 elfloader is already an EFI PE/COFF binary, but L4TLauncher may
  need the image in a specific format or location
- Need to investigate L4TLauncher's `ProcessExtLinuxConfig()` to see if it
  can load arbitrary EFI applications or only Linux kernels

Source: `~/nvidia-uefi-docker/nvidia-uefi/edk2-nvidia/Silicon/NVIDIA/Application/L4TLauncher/L4TLauncher.c`

### Option 3: Minimal shell usage

Avoid heavy shell operations before launching seL4. Use the autopilot's
TFTP/SSH deployment instead of manual shell commands. This is already the
default for automated testing.

### Option 4: Fix the UEFI bug

Debug the heap corruption in UEFI's shell/filesystem drivers. Would require:
1. Flashing `uefi_Jetson_DEBUG.bin` for verbose output
2. Reproducing the `ls` + launch sequence with debug serial capture
3. Tracing the stale pointer to its source
4. Likely a bug in `ShellPkg` or `FatPkg` heap management

## Relationship to seL4 RAS Fixes

This UEFI bug is completely independent of the seL4 kernel and elfloader
RAS fixes. The kernel fixes (frame cap slot identity, PT parent slot identity)
and the elfloader quiesce (`quiesce_uefi_hyp`) address different root causes:

- **Kernel RAS**: stale page table descriptors from vspace teardown → fixed
- **Elfloader RAS**: stale UEFI TLB entries after ExitBootServices → fixed
- **UEFI RAS**: heap corruption from heavy shell usage → this bug, unfixed

The automated test runs (autopilot) that don't go through the UEFI shell
show zero RAS errors end-to-end.
