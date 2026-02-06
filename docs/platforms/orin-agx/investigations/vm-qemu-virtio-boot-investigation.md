# vm_qemu_virtio Boot Hang Investigation

## Problem Summary

vm_minimal boots successfully on Orin AGX with BPMP working, but vm_qemu_virtio hangs even when:
1. VM1 is removed entirely
2. DeclareVirtiosoCAmkESVM is changed to DeclareCAmkESARMVM

This means the issue is NOT in TII templates/modules - it's in the camkes configuration itself.

## Symptom

- VM0 boots, switches console, but no TCU output after switch
- Only 1 IRQ 208 (HSP Top0 doorbell for BPMP) injection seen
- No BPMP success message (`tegra-bpmp bpmp: firmware:...`)

## Configuration Differences Found

### 1. CNode Size (POTENTIAL ROOT CAUSE)

| App | cnode_size_bits | Slots |
|-----|-----------------|-------|
| vm_minimal | 18 | 256K |
| vm_qemu_virtio | 23 | 8M |

**Critical:** vm_minimal has this comment:
```c
/*
 * TODO: changing cnode_size_bits from 23 -> 18 solved
 * the 2M frame mapping issue on RPi4. Figure out why.
 */
```

This suggests cnode_size_bits=23 can cause issues with frame mapping.

**Files:**
- vm_minimal: `projects/vm-examples/apps/Arm/vm_minimal/vm_minimal.camkes:35`
- vm_qemu_virtio: `projects/virtioso-camkes-vm/apps/Arm/vm_qemu_virtio/vm_qemu_virtio.camkes:59`

### 2. Priority Settings

| App | base_prio | _priority |
|-----|-----------|-----------|
| vm_minimal | 100 | 101 |
| vm_qemu_virtio | 100 | 121 |

vm_minimal gets these from `VM_CONFIGURATION_DEF(0)` macro.
vm_qemu_virtio sets them explicitly.

### 3. Heap Size

| App | heap_size |
|-----|-----------|
| vm_minimal | 0x300000 (3MB) |
| vm_qemu_virtio | 0x5000000 (80MB) |

vm_qemu_virtio needs larger heap for LZ4 kernel decompression.

### 4. Configuration Macro

| App | Macro |
|-----|-------|
| vm_minimal | `VM_CONFIGURATION_DEF(0)` |
| vm_qemu_virtio | `VM_TII_CONFIGURATION_DEF(0)` |

`VM_CONFIGURATION_DEF` sets:
```c
vm##num.fs_shmem_size = 0x100000;
vm##num.global_endpoint_base = 1 << 27;
vm##num.asid_pool = true;
vm##num.simple = true;
vm##num.base_prio = 100;
vm##num._priority = 101;
vm##num.sem_value = 0;
vm##num.heap_size = 0x300000;
```

`VM_TII_CONFIGURATION_DEF` sets:
```c
vm##num.fs_shmem_size = 0x100000;
vm##num.global_endpoint_base = 1 << 27;
vm##num.asid_pool = true;
vm##num.simple = true;
vm##num.sem_value = 0;
// Does NOT set base_prio, _priority, heap_size
```

### 5. Include Files

| App | Include |
|-----|---------|
| vm_minimal | `#include <configurations/vm.h>` |
| vm_qemu_virtio | `#include <configurations/virtioso/vm.h>` |

Note: virtioso/vm.h includes vm.h, so all standard macros are available.

### 6. Kernel Image Name

| App | kernel_name |
|-----|-------------|
| vm_minimal | `"linux"` |
| vm_qemu_virtio | `"linux.lz4"` |

vm_qemu_virtio uses LZ4-compressed kernel.

### 7. CMakeLists.txt Architecture

**vm_minimal:**
```cmake
include(${CAMKES_ARM_VM_HELPERS_PATH})
# Does NOT call DeclareCAmkESARMVM - uses pre-built VM component
DeclareCAmkESRootserver(vm_minimal.camkes ...)
```

**vm_qemu_virtio:**
```cmake
include(${VIRTIOSO_CAMKES_VM_HELPERS_PATH})  # Includes CAMKES_ARM_VM_HELPERS_PATH
DeclareCAmkESARMVM(VM0)  # Explicitly declares VM0 component
DeclareCAmkESRootserver(vm_qemu_virtio.camkes ...)
```

Key insight: vm_minimal imports `<VM_Arm/VM.camkes>` which pre-defines a `VM` component.
The VM_Arm/CMakeLists.txt calls `DeclareCAmkESARMVM(VM)` for this pre-defined component.

vm_qemu_virtio defines its own `component VM0 { VM_TII_INIT_DEF() }` and calls
`DeclareCAmkESARMVM(VM0)` separately.

### 8. Component Definition

**vm_minimal:**
```camkes
import <VM_Arm/VM.camkes>;  // Pre-defined component: component VM { VM_INIT_DEF() }
assembly {
    composition {
        VM_COMPOSITION_DEF(0)  // Uses 'VM' component type, creates 'vm0' instance
    }
}
```

**vm_qemu_virtio:**
```camkes
// Does NOT import VM_Arm/VM.camkes
component VM0 {
    VM_TII_INIT_DEF()  // Custom component with TII extensions
}
assembly {
    composition {
        VM_COMPOSITION_DEF(0)  // Uses 'VM0' component type
    }
}
```

Note: `VM_TII_INIT_DEF()` expands `VM_INIT_DEF()` and adds:
```c
attribute int tracebuffer_base;
attribute int tracebuffer_size;
attribute int ramoops_base;
attribute int ramoops_size;
attribute { ... } vm_virtio_driver_channels[] = [];
attribute { ... } vm_virtio_device_channels[] = [];
```

### 9. Kernel Image Loading

**vm_minimal:**
```cmake
AddToFileServer("linux" "${CAMKES_VM_IMAGES_DIR}/orinagx/linux")
AddToFileServer("linux-initrd" ${rootfs_file})
```

**vm_qemu_virtio:**
```cmake
AddToFileServerCompressed("linux" "${VM_IMAGE_LINUX}")  # Different function!
AddToFileServer("linux-initrd" ${VM_IMAGE_INITRD})
```

`AddToFileServerCompressed` is a TII function that handles LZ4 compression.
This may affect how the kernel is stored and loaded.

### 10. CAmkES Import Structure

**vm_minimal.camkes:**
```camkes
import <VM_Arm/VM.camkes>;  // This imports the pre-defined VM component
import <devices.camkes>;
```

**vm_qemu_virtio.camkes:**
```camkes
// Does NOT import VM_Arm/VM.camkes
import <seL4VMDTBPassthrough.idl4>;
import <FileServerInterface.camkes>;
import <FileServer/FileServer.camkes>;
import <SerialServer/SerialServer.camkes>;
import <TimeServer/TimeServer.camkes>;
import <devices.camkes>;
```

Note: VM_Arm/VM.camkes internally imports FileServer, SerialServer, TimeServer, so both
should end up with similar components. But vm_qemu_virtio does explicit imports.

### 11. TII VM Configuration Override

TII's vm.h **undefines and redefines** VM_COMPONENT_DEF:
```c
#undef VM_COMPONENT_DEF
#define VM_COMPONENT_DEF(num) \
    component VM##num vm##num;
```

This means:
- **Standard vm.h**: `VM_COMPOSITION_DEF(0)` → `component VM vm0;` (generic type)
- **TII vm.h**: `VM_COMPOSITION_DEF(0)` → `component VM0 vm0;` (specific type)

This requires vm_qemu_virtio to define its own `component VM0 { ... }`.

### 12. Platform FDT Hook (Same for Both)

Both applications use the same `fdt_plat_customize()` from:
`projects/vm/components/VM_Arm/src/modules/plat/orinagx/fdt.c`

This hook:
1. Sets interrupt-parent at root (GIC phandle)
2. Adds PMC node for GPIO hierarchical IRQ mode
3. Adds MAC address for MGBE0

This should be identical for both apps.

### 13. is_sram_address() Modification (Same for Both)

User modified `main.c:208-217` to always return false:
```c
static bool is_sram_address(uintptr_t addr)
{
    return false;  // User added - disables SRAM cacheability
#if defined(CONFIG_PLAT_ORIN_AGX)
    return (addr >= 0x40000000 && addr < 0x40080000);
#else
    return false;
#endif
}
```

This affects both apps equally and is confirmed working in vm_minimal.

### 14. DTB/Initrd Address Calculation

**vm_minimal:**
```c
#define VM_RAM_BASE     0x90000000
#define VM_RAM_SIZE     0x20000000   /* 512 MB */
#define VM_DTB_ADDR     0xAE000000   // Fixed address
#define VM_INITRD_ADDR  0xAD000000   // Fixed address
#define VM_ENTRY_ADDR   0x90200000   // Fixed address
```

**vm_qemu_virtio:**
```c
#define VM0_RAM_BASE    0x90000000
#define VM0_RAM_SIZE    0x20000000      /* 512 MB */
#define VM_DTB_MAX_SIZE 0x1000000       /* 16 MB */
#define VM_INITRD_MAX_SIZE 0x2000000    /* 32 MB */
#define VM0_DTB_ADDR    VM0_RAM_BASE + VM0_RAM_SIZE - VM_DTB_MAX_SIZE     // = 0xAF000000
#define VM0_INITRD_ADDR VM0_DTB_ADDR - VM_INITRD_MAX_SIZE                  // = 0xAD000000
#define VM0_ENTRY_ADDR  VM0_RAM_BASE + 0x200000                            // = 0x90200000
```

**Calculated addresses:**
| Address | vm_minimal | vm_qemu_virtio |
|---------|------------|----------------|
| RAM | 0x90000000-0xB0000000 | 0x90000000-0xB0000000 |
| DTB | 0xAE000000 | 0xAF000000 |
| Initrd | 0xAD000000 | 0xAD000000 |
| Entry | 0x90200000 | 0x90200000 |

The addresses are nearly identical. DTB differs by 16MB (vm_qemu_virtio 16MB higher).
Initrd and entry addresses are the same.

**Unlikely to be the cause** - addresses are equivalent within the same RAM region.

### 15. vm0.irqs Configuration

**vm_minimal:** Does NOT set `vm0.irqs` (uses default)

**vm_qemu_virtio:** `vm0.irqs = [];` explicitly set to empty

This difference may affect IRQ routing.

### 16. provide_dtb Setting

**vm_minimal:** Not set (defaults to `true` from vm.h)

**vm_qemu_virtio:** `"provide_dtb" : false`

Both have `generate_dtb: true`, so DTB should be generated in both cases.

### 17. Boot Command Line

**vm_minimal:**
```
console=ttyTCU0 earlycon=pl011,mmio32,0x31d0000 debug loglevel=8 ignore_loglevel printk.devkmsg=on rdinit=/init arm64.nopauth maxcpus=1
```

**vm_qemu_virtio:**
```
console=ttyTCU0 earlycon=pl011,mmio32,0x31d0000 debug loglevel=8 ignore_loglevel printk.devkmsg=on rdinit=/init arm64.nopauth maxcpus=1 swiotlb=512
```

Only difference: vm_qemu_virtio adds `swiotlb=512` (SWIOTLB bounce buffer).

### 18. CRITICAL: Mismatch Between CMakeLists.txt and Camkes File

**After user's change to DeclareCAmkESARMVM:**

CMakeLists.txt:
```cmake
DeclareCAmkESARMVM(VM0)  # Compiles standard VM_Arm sources
```

But vm_qemu_virtio.camkes STILL uses TII macros:
```camkes
#include <configurations/virtioso/vm.h>

component VM0 {
    VM_TII_INIT_DEF()         // TII macro - expects TII attributes!
}

configuration {
    VM_TII_CONFIGURATION_DEF(0)  // TII macro!
}
```

**This is a mismatch:**
- CAmkES generates code expecting TII attributes:
  - `tracebuffer_base`, `tracebuffer_size`
  - `ramoops_base`, `ramoops_size`
  - `vm_virtio_driver_channels[]`, `vm_virtio_device_channels[]`
- But DeclareCAmkESARMVM doesn't compile TII sources that would USE these attributes

**The test should either:**
1. Change camkes file to use `VM_INIT_DEF()` and `VM_CONFIGURATION_DEF(0)`, OR
2. Keep DeclareVirtiosoCAmkESVM in CMakeLists.txt

## Summary of Most Likely Culprits

1. **MISMATCH: TII camkes macros + DeclareCAmkESARMVM** - Incompatible combination
2. **cnode_size_bits: 23 vs 18** - vm_minimal comment says 18 fixed 2MB frame issue on RPi4
3. **Include header** - `configurations/virtioso/vm.h` redefines VM_COMPONENT_DEF
4. **vm0.irqs = []** - explicit empty array vs default (may affect IRQ routing)
5. **Component architecture** - vm_qemu_virtio defines `component VM0` separately vs vm_minimal using pre-defined `VM`

## Differences Still To Investigate

- [ ] Generated capdl spec differences (compare frame/IRQ allocations)
- [ ] Any effect of TII #undef VM_COMPONENT_DEF on CAmkES processing
- [ ] Whether larger cnode_size_bits (23 vs 18) causes allocation issues
- [ ] Whether DTB/initrd address differences affect boot

## Test Plan (Priority Order)

1. **FIX MISMATCH: Make camkes file match CMakeLists.txt** - Either:
   - Option A: Change camkes to use `VM_INIT_DEF()` + `VM_CONFIGURATION_DEF(0)` + `#include <configurations/vm.h>`, OR
   - Option B: Revert CMakeLists.txt to use `DeclareVirtiosoCAmkESVM(VM0)`
2. **Test cnode_size_bits = 18** - Most likely fix based on vm_minimal TODO comment
3. **Remove vm0.irqs = []** - Match vm_minimal (don't explicitly set)
4. **Import VM_Arm/VM.camkes** - Use pre-defined VM component like vm_minimal
5. **Test _priority = 101** - Match vm_minimal priority (currently 121)
6. **Use uncompressed kernel** - Use "linux" instead of "linux.lz4"

## Files Reference

### vm_minimal
- Main: `projects/vm-examples/apps/Arm/vm_minimal/vm_minimal.camkes`
- Devices: `projects/vm-examples/apps/Arm/vm_minimal/orinagx/devices.camkes`

### vm_qemu_virtio
- Main: `projects/virtioso-camkes-vm/apps/Arm/vm_qemu_virtio/vm_qemu_virtio.camkes`
- Devices: `projects/virtioso-camkes-vm/apps/Arm/vm_qemu_virtio/orinagx/devices.camkes`

### Configuration Headers
- Standard: `projects/vm/components/VM_Arm/configurations/vm.h`
- TII: `projects/virtioso-camkes-vm/configurations/virtioso/vm.h`
