# Hypervisor Ftrace Control Interface

## Overview

The Hypervisor Ftrace Control Interface (`hyp_ftrace`) provides a mechanism for guest Linux running under seL4 to control the kernel's ftrace debugging facility. This enables precise capture of kernel activity triggered by guest operations, which is invaluable for debugging hypervisor interactions, device emulation, and performance analysis.

## Motivation

When debugging seL4 kernel behavior triggered by guest VM activity (e.g., MMIO faults, hypercalls, interrupt injection), traditional kernel debugging approaches face challenges:

1. **Timing**: Ftrace captures everything from boot, filling the buffer before interesting events occur
2. **Precision**: No way to start/stop tracing at specific guest execution points
3. **Correlation**: Difficult to correlate guest events with kernel trace data

The hyp_ftrace interface solves these by allowing the guest to:
- **Arm** ftrace at a known point (e.g., before probing a device driver)
- **Dump** ftrace data after the interesting activity completes

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        Guest Linux (VM)                                 │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  hyp-ftrace.ko kernel module                                      │  │
│  │    - Probes "sel4,hyp-ftrace" DT node                            │  │
│  │    - Maps MMIO region at 0x0F010000                              │  │
│  │    - Exports hyp_ftrace_arm() / hyp_ftrace_dump()                │  │
│  │    - Auto-arms on probe (configurable)                           │  │
│  └──────────────────────────┬────────────────────────────────────────┘  │
│                             │ MMIO write                                │
│                             ▼                                           │
├─────────────────────────────────────────────────────────────────────────┤
│                        VMM (CAmkES component)                           │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  hyp_ftrace.c fault handler                                       │  │
│  │    - Registered via vm_reserve_memory_at()                        │  │
│  │    - Intercepts guest MMIO to 0x0F010000                         │  │
│  │    - Decodes command register writes                              │  │
│  │    - Calls seL4 benchmark syscalls                                │  │
│  └──────────────────────────┬────────────────────────────────────────┘  │
│                             │ seL4 syscall                              │
│                             ▼                                           │
├─────────────────────────────────────────────────────────────────────────┤
│                        seL4 Microkernel                                 │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  benchmark.c syscall handlers                                     │  │
│  │    - seL4_BenchmarkResetLog() → ftrace_reset()                   │  │
│  │    - seL4_BenchmarkFinalizeLog() → ftrace_dump_binary()          │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  ftrace.c ring buffer                                             │  │
│  │    - Circular buffer of kernel events                             │  │
│  │    - LZ4-compressed binary dump to UART                          │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

## Register Interface

The hyp_ftrace device exposes a simple MMIO register interface:

| Offset | Name   | Access | Description |
|--------|--------|--------|-------------|
| 0x00   | CMD    | W      | Command register - write to execute command |
| 0x04   | STATUS | R      | Status register - always returns 0 (idle) |

### Commands

| Value | Name | Description |
|-------|------|-------------|
| 0x01  | ARM  | Clear ftrace buffer and enable recording |
| 0x02  | DUMP | Dump ftrace buffer to UART (LZ4 compressed) |

### MMIO Address

The device is mapped at physical address `0x0F010000`, chosen because:
- Located in unused device address space on Orin AGX
- Above standard peripherals (e.g., PL011 at 0x09000000)
- Below RAM regions
- Single 4KB page is sufficient

## Implementation Details

### 1. seL4 Kernel (`kernel/src/benchmark/benchmark.c`)

The kernel's benchmark syscall handlers are extended to support ftrace:

```c
exception_t handle_SysBenchmarkResetLog(void)
{
    // ... existing benchmark reset code ...

#ifdef CONFIG_FTRACE_BUFFER
    /* Reset ftrace buffer when benchmark log is reset */
    ftrace_reset();
#endif

    return EXCEPTION_NONE;
}

exception_t handle_SysBenchmarkFinalizeLog(void)
{
    // ... existing benchmark finalize code ...

#ifdef CONFIG_FTRACE_BUFFER
    /* Dump ftrace buffer when benchmark log is finalized */
    ftrace_dump_binary();
#endif

    return EXCEPTION_NONE;
}
```

**Key change**: The guard was changed from `CONFIG_KERNEL_FUNCTION_TRACE` to `CONFIG_FTRACE_BUFFER`, enabling ftrace dump in `el2-ras` mode without requiring full function tracing overhead.

### 2. VMM Module (`projects/virtioso-camkes-vm/src/camkes/modules/hyp_ftrace.c`)

The VMM intercepts guest MMIO accesses and translates them to seL4 syscalls:

```c
static memory_fault_result_t hyp_ftrace_fault_handler(vm_t *vm, vm_vcpu_t *vcpu,
                                                       uintptr_t paddr, size_t len,
                                                       void *cookie)
{
    hyp_ftrace_t *hf = cookie;
    uintptr_t offset = paddr - hf->base;

    if (is_vcpu_read_fault(vcpu)) {
        // Handle reads (STATUS register)
        set_vcpu_fault_data(vcpu, 0);  // Always idle
    } else {
        // Handle writes (CMD register)
        seL4_Word value = emulate_vcpu_fault(vcpu, 0);
        switch (value) {
        case CMD_ARM:
            seL4_BenchmarkResetLog();
            break;
        case CMD_DUMP:
            seL4_BenchmarkFinalizeLog();
            break;
        }
    }

    advance_vcpu_fault(vcpu);
    return FAULT_HANDLED;
}
```

The module gracefully handles the case where benchmarks are disabled:

```c
#ifdef CONFIG_ENABLE_BENCHMARKS
    seL4_BenchmarkResetLog();
#else
    ZF_LOGW("hyp_ftrace: ARM command ignored (benchmarks disabled)");
#endif
```

### 3. CAmkES Template (`templates/hyp_ftrace.template.c`)

The template instantiates the module when configured:

```c
/*- set hyp_ftrace = configuration[me.name].get('hyp_ftrace') -*/
/*- if hyp_ftrace is not none -*/
static hyp_ftrace_t hyp_ftrace = {
    .base = /*? hyp_ftrace ?*/,
    .size = BIT(PAGE_BITS_4K),
};

DEFINE_MODULE(hyp_ftrace, &hyp_ftrace, hyp_ftrace_init)
/*- endif -*/
```

### 4. Device Tree Generation (`src/plat/orinagx/fdt.c`)

The platform FDT customization hook adds the device node:

```c
static int fdt_add_hyp_ftrace(void *fdt)
{
    int node = fdt_add_subnode(fdt, root, "hypervisor-ftrace@f010000");

    fdt_setprop_string(fdt, node, "compatible", "sel4,hyp-ftrace");

    uint64_t reg[] = { cpu_to_fdt64(0x0F010000), cpu_to_fdt64(0x1000) };
    fdt_setprop(fdt, node, "reg", reg, sizeof(reg));

    fdt_setprop_string(fdt, node, "status", "okay");

    return 0;
}
```

### 5. Guest Linux Driver (`drivers/platform/sel4/hyp-ftrace.c`)

The driver is built into the kernel (not a loadable module) to ensure early initialization:

```c
static void __iomem *hyp_ftrace_base;

void hyp_ftrace_arm(void)
{
    if (!hyp_ftrace_available)
        return;
    writel_relaxed(CMD_ARM, hyp_ftrace_base + HYP_FTRACE_CMD);
}
EXPORT_SYMBOL_GPL(hyp_ftrace_arm);

void hyp_ftrace_dump(void)
{
    if (!hyp_ftrace_available)
        return;
    writel_relaxed(CMD_DUMP, hyp_ftrace_base + HYP_FTRACE_CMD);
}
EXPORT_SYMBOL_GPL(hyp_ftrace_dump);

static int __init hyp_ftrace_init(void)
{
    hyp_ftrace_base = ioremap(HYP_FTRACE_BASE, HYP_FTRACE_SIZE);
    if (!hyp_ftrace_base)
        return 0;  /* Not running under seL4 */

    hyp_ftrace_available = true;
    hyp_ftrace_arm();  /* Auto-arm on init */
    return 0;
}
postcore_initcall(hyp_ftrace_init);
```

Key design decisions:
- **Built-in driver**: Uses `postcore_initcall` (level 2) to initialize before BPMP and other device drivers
- **Fixed address**: Hardcoded 0x0F010000 since we control both VMM and guest
- **Graceful fallback**: ioremap failure is not an error - just means not running under seL4
- **Auto-arm**: Automatically arms ftrace on init to capture early boot activity

## Configuration

### CAmkES Configuration

Enable hyp_ftrace in `devices.camkes`:

```camkes
assembly {
    configuration {
        vm0.hyp_ftrace = 0x0F010000;
    }
}
```

### Kernel Configuration

Enable benchmark syscalls in defconfig:

```
CONFIG_BENCHMARK_GENERIC=y
```

This sets `CONFIG_ENABLE_BENCHMARKS`, making the benchmark syscalls available.

For ftrace buffer support:

```
CONFIG_KERNEL_FTRACE_BUFFER=y
```

### Guest Kernel Configuration

The guest kernel must have the built-in driver enabled:

```
CONFIG_SEL4_PLATFORM=y
CONFIG_SEL4_HYP_FTRACE=y
```

This is applied via the `hyp-ftrace.cfg` fragment in the Yocto kernel recipe.

### Build Configurations

| Config | Benchmarks | Ftrace Buffer | Function Trace |
|--------|------------|---------------|----------------|
| `orinagx_defconfig` | Yes | No | No |
| `orinagx_ras_defconfig` | Yes* | Yes | No |
| `orinagx_ftrace_defconfig` | Yes* | Yes | Yes |

*These configs should have `CONFIG_BENCHMARK_GENERIC=y` added for hyp_ftrace support.

## Usage

### Automatic Arming

By default, the guest kernel module arms ftrace when it probes:

```
[    1.234567] hyp-ftrace platform: initialized at [mem 0x0f010000-0x0f010fff]
[    1.234789] hyp-ftrace platform: auto-arming ftrace
```

### Manual Control from Guest

Other kernel modules can call the exported functions:

```c
#include <linux/module.h>

extern void hyp_ftrace_arm(void);
extern void hyp_ftrace_dump(void);

static int my_driver_probe(struct platform_device *pdev)
{
    hyp_ftrace_arm();  // Start fresh trace

    // ... do interesting work ...

    hyp_ftrace_dump(); // Dump trace to UART
    return 0;
}
```

### Userspace Control (Future)

A sysfs interface could be added:

```bash
# Arm ftrace
echo 1 > /sys/devices/platform/hypervisor-ftrace@f010000/arm

# Dump ftrace
echo 1 > /sys/devices/platform/hypervisor-ftrace@f010000/dump
```

## Data Flow

```
1. Guest writes 0x01 to 0x0F010000
   │
   ▼
2. Stage 2 page fault (unmapped address)
   │
   ▼
3. VMM fault handler invoked
   │
   ▼
4. hyp_ftrace_fault_handler() called
   │
   ▼
5. seL4_BenchmarkResetLog() syscall
   │
   ▼
6. Kernel ftrace_reset() clears buffer
   │
   ▼
7. Fault handler advances guest PC
   │
   ▼
8. Guest continues execution
```

## Output Format

When DUMP is triggered, the kernel outputs LZ4-compressed binary data:

```
FTRACE: SysBenchmarkFinalizeLog called, dumping ftrace...
=== FTRACE BINARY DUMP START ===
[LZ4 compressed data in base91 encoding]
=== FTRACE BINARY DUMP END ===
```

Decode with:
```bash
kernel/tools/decode_ftrace_binary.py results/console/<profile-defined-log> \
    --kernel orinagx_vm_qemu_virtio/kernel/kernel.elf
```

## Security Considerations

1. **Privilege**: Only the VMM can invoke benchmark syscalls; the guest cannot directly call them
2. **Isolation**: Each VM has its own hyp_ftrace instance; no cross-VM interference
3. **Resource**: Ftrace buffer is kernel-global; multiple VMs share the same buffer
4. **DoS**: A malicious guest could spam ARM/DUMP commands; rate limiting could be added

## Files Summary

### seL4 Kernel
| File | Purpose |
|------|---------|
| `kernel/src/benchmark/benchmark.c` | Syscall handlers with ftrace integration |

### VMM (CAmkES)
| File | Purpose |
|------|---------|
| `projects/virtioso-camkes-vm/src/camkes/modules/hyp_ftrace.c` | MMIO fault handler |
| `projects/virtioso-camkes-vm/include/virtioso/camkes/hyp_ftrace.h` | Header file |
| `projects/virtioso-camkes-vm/templates/hyp_ftrace.template.c` | CAmkES template |
| `projects/virtioso-camkes-vm/src/plat/orinagx/fdt.c` | Device tree generation |
| `projects/virtioso-camkes-vm/virtioso_camkes_vm_helpers.cmake` | Build integration |

### Guest Linux (Built-in Driver)
| File | Purpose |
|------|---------|
| `.../linux/linux-jammy-nvidia-tegra/0001-Add-seL4-hyp-ftrace-driver.patch` | Kernel patch |
| `.../linux/linux-jammy-nvidia-tegra/hyp-ftrace.cfg` | Kernel config fragment |
| `.../linux/linux-jammy-nvidia-tegra_%.bbappend` | Yocto recipe |

### Configuration
| File | Purpose |
|------|---------|
| `virtioso-build/configs/orinagx_defconfig` | seL4 kernel config with benchmarks |
| `projects/vm-examples/.../orinagx/devices.camkes` | CAmkES VM configuration |

## Future Enhancements

1. **Sysfs Interface**: Allow userspace control without kernel module modifications
2. **Event Notifications**: Notify guest when dump completes
3. **Filter Configuration**: Allow guest to select which events to trace
4. **Per-VM Buffers**: Separate ftrace buffers per VM for isolation
5. **Trigger Points**: Define specific events that auto-dump (e.g., RAS error)
