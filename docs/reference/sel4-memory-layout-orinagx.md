# seL4 Memory Layout on Orin AGX

This document describes the physical memory layout of seL4 on NVIDIA Orin AGX, including how the kernel load address is determined and what resides in different memory regions.

## Key Concepts

### Memory Address Determination

The kernel's physical load address is determined by the **DTS memory@ node**, not by IMAGE_START_ADDR:

1. **DTS memory@ node** defines where RAM starts for seL4
2. **Kernel ELF** is compiled with load addresses (LMA) matching the DTS
3. **IMAGE_START_ADDR** only affects where UEFI loads the elfloader binary
4. **Elfloader** extracts the kernel to the address specified in the kernel ELF

### Build Configurations

| Config | DTS memory start | Kernel LMA | IMAGE_START_ADDR | First 200KB |
|--------|-----------------|------------|------------------|-------------|
| Normal (`orinagx_defconfig`) | 0x80000000 | 0x80000000 | 0x80010000 | Kernel code |
| Diagnostic (`orinagx_diag_defconfig`) | 0x80032000 | 0x80032000 | 0x80032000 | Device untyped |

## Normal Mode Memory Layout (RAM at 0x80000000)

When using `orinagx_defconfig` or `orinagx_ftrace_defconfig`:

```
Physical Address    Section      Size    Contents
─────────────────────────────────────────────────────────────
0x80000000         .boot        64KB    Kernel boot code
  0x80000000       _start               Entry point
  0x80000070       init_pt_with_safe_ptes
  0x80003750       init_kernel

0x80010000         .boot.bss    ~8KB    Boot data structures
  0x80010000       rootserver
  0x800100b8       ndks_boot

0x80012000         .text        ...     Main kernel code
  0x80012000       ki_boot_end          End of boot section
  0x80020000       arm_vector_table     Exception vectors (LIVE!)
  0x80020784       invalid_vector_entry
  0x8002080c       lower_el_sync
  ...

0x80048000         .bss         2.1MB   Kernel BSS (page tables, etc.)
  includes armKSGlobalKernelPGD, armKSGlobalUserVSpace
```

### Critical: First 200KB Contains Live Code

In normal mode, **0x80000000-0x80032000 contains actively-used kernel code**:

- **Boot code** (0x80000000-0x80010000): Used during initialization
- **Boot BSS** (0x80010000-0x80012000): Boot-time data structures
- **Kernel .text** (0x80012000-0x80032000): **Live kernel code** including:
  - `arm_vector_table` at 0x80020000 - used for EVERY interrupt/exception
  - Exception handlers (`cur_el_sync`, `lower_el_sync`, etc.)
  - Syscall handlers

This memory is **not scratch space** - corrupting it would crash the kernel.

## Diagnostic Mode Memory Layout (RAM at 0x80032000)

When using `orinagx_diag_defconfig`:

```
Physical Address    Contents
─────────────────────────────────────────────────────────────
0x80000000-0x80032000  Device untyped (200KB diagnostic region)
                       - Initialized by elfloader with pattern
                       - Verified periodically by ftrace
                       - NOT used by kernel

0x80032000         .boot        56KB    Kernel boot code
0x80040000         .boot.bss    ~8KB    Boot data structures
0x80042000         .text        ...     Main kernel code
...
```

### How Diagnostic Mode Works

1. **DTS**: `orinagx-diag.dts` declares memory starting at 0x80032000
2. **Kernel**: Compiled with LMA=0x80032000
3. **IMAGE_START_ADDR**: Set to 0x80032000
4. **Elfloader**: Initializes 0x80000000-0x80032000 with pattern `0xDEADBEEFCAFEBABE ^ index`
5. **seL4**: Sees 0x80000000-0x80032000 as "device untyped" (not RAM)
6. **Ftrace**: Periodically verifies the pattern to detect corruption

## Examining the Kernel Binary

### Check kernel load address:
```bash
aarch64-linux-gnu-objdump -h kernel/kernel.elf | head -10
```

Example output (normal mode):
```
Idx Name          Size      VMA               LMA               File off
  0 .boot         00010000  0000008080000000  0000000080000000  000000f0
  1 .boot.bss     00001270  0000008080010000  0000000080010000  000100f0
  2 .text         00024b58  0000008080012000  0000000080012000  00012000
```

- **VMA** = Virtual Memory Address (kernel virtual address space at 0x8080000000)
- **LMA** = Load Memory Address (physical address where code is loaded)

### List symbols in first 200KB:
```bash
aarch64-linux-gnu-nm kernel/kernel.elf | sort | awk '$1 ~ /^00000080800[0123]/'
```

### Check DTS memory region:
```bash
cat kernel/kernel.dts | grep -A5 "memory@"
```

### Check IMAGE_START_ADDR:
```bash
cat elfloader/gen_headers/image_start_addr.h
```

## Boot Sequence

1. **UEFI** loads elfloader EFI binary at IMAGE_START_ADDR
2. **Elfloader** starts executing:
   - Creates boot page tables for identity mapping
   - Reads kernel.elf from embedded CPIO archive
   - Gets kernel physical bounds from ELF headers (LMA)
   - Loads kernel to its LMA (e.g., 0x80000000 or 0x80032000)
   - Loads DTB and user images
   - Jumps to kernel `_start`
3. **Kernel** starts at `_start`:
   - Initializes page tables (including safe PTEs)
   - Sets up kernel virtual address space (0x8080000000 → 0x80000000)
   - Initializes interrupts, timers, etc.
   - Creates initial thread and starts scheduling

## Relationship Between Addresses

```
Kernel Virtual Address = Physical Address + KERNEL_OFFSET
                       = Physical Address + 0x8000000000

Example:
  _start virtual:  0x8080000000
  _start physical: 0x0080000000
  KERNEL_OFFSET:   0x8000000000
```

The kernel runs with virtual addresses (0x8080xxxxxxxx) but physical memory starts at 0x80000000 (or 0x80032000 in diagnostic mode).

## Configuration Files

| File | Purpose |
|------|---------|
| `kernel/tools/dts/orinagx.dts` | Normal mode DTS (RAM at 0x80000000) |
| `kernel/tools/dts/orinagx-diag.dts` | Diagnostic mode DTS (RAM at 0x80032000) |
| `kernel/src/plat/orinagx/config.cmake` | Platform config, KernelOrinAGXDiagRegion option |
| `tools/seL4/cmake-tool/helpers/application_settings.cmake` | IMAGE_START_ADDR logic |
| `tii_sel4_build/scripts/build_sel4.sh` | DIAG_REGION build variable |

## Build Commands

```bash
# Normal mode (RAM at 0x80000000, kernel at 0x80000000)
make orinagx_defconfig && make sel4test

# Ftrace mode (same memory layout as normal)
make orinagx_ftrace_defconfig && make sel4test

# Diagnostic mode (RAM at 0x80032000, 200KB reserved for pattern)
make orinagx_diag_defconfig && make sel4test
```

## See Also

- [Diagnostic Region Investigation](diagnostic-region-investigation.md) - RAS error debugging
- [Orin RAS Error Investigation](orin-ras-error-investigation.md) - Full RAS error analysis
- [Tegra Cache Operations](tegra-cache-operations.md) - dc civac vs dc cisw
