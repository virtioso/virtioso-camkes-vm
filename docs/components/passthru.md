# CAmkES Device Tree Passthrough Analysis

## Overview

This document analyzes how CAmkES handles device tree (DTB) generation for VMs, focusing on the `dtb()` macro in `devices.camkes` and the fdtgen library.

## Summary (2026-01-05)

**CONFIRMED: Kernel DTB properties and child nodes DO reach the VM correctly.**

Test run `20260105-165844` (clean build without debug code) verified:
```
=== Ethernet DT properties ===
Properties in ethernet node:
... nvidia,ptp-rx-queue ... nvidia,mtl-queues ... nvidia,num-mtl-queues ... stmmac-axi-config ...
ptp-rx-queue: 00000000
mtl-queues: 00000000
num-mtl-queues: 00000001
num-dma-chans: 00000001
compatible: nvidia,tegra234-mgbe
=== End Ethernet DT ===
```

**What fdtgen passes through correctly:**
- All `nvidia,*` properties from kernel DTB ✓
- Child nodes like `stmmac-axi-config` ✓
- All properties of nodes in `plat_keep_device_and_subtree` ✓

**What fdt_plat_customize() must add:**
- `/chosen/nvidia,ether-mac0` - MAC address (driver fails without this)
- `nvidia,vm-irq-config` - VM interrupt configuration
- Any VM-specific properties not in kernel DTB

**Driver status:** MGBE driver probes successfully when MAC address is provided, but fails on `ether_open()` with "PTP RX queue not enabled" - this is a hardware configuration issue, not DTB passthrough.

## DTB Flow in CAmkES

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         DTB Generation Flow                              │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  1. orinagx.dts ──(dtc)──> kernel.dtb                                   │
│         │                       │                                        │
│         │                       ▼                                        │
│         │              seL4 kernel embeds DTB                           │
│         │                       │                                        │
│         │                       ▼                                        │
│         │              bootinfo FDT passed to userspace                 │
│         │                       │                                        │
│         │                       ▼                                        │
│  2. devices.camkes ────> camkes_io_fdt() returns bootinfo FDT           │
│     dtb([...paths...])          │                                        │
│         │                       ▼                                        │
│         │              fdtgen processes DTB                              │
│         │                       │                                        │
│         │                       ▼                                        │
│  3.     └──────────────> fdtgen_keep_nodes() ◄── PROBLEM HERE!          │
│                                 │                                        │
│                                 ▼                                        │
│                          trim_tree() deletes                             │
│                          nodes NOT in keep list                          │
│                                 │                                        │
│                                 ▼                                        │
│  4. fdt_plat_customize() ──> Adds platform-specific nodes               │
│                                 │                                        │
│                                 ▼                                        │
│                          Final VM DTB                                    │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

## Key Code Locations

| File | Function | Purpose |
|------|----------|---------|
| `projects/camkes-tool/libsel4camkes/src/io.c` | `camkes_io_fdt()` | Returns bootinfo FDT to VM |
| `projects/vm/components/VM_Arm/src/main.c:897` | DTB generation | Calls fdtgen with dtb() paths |
| `projects/projects_libs/libfdtgen/fdtgen.c` | `fdtgen_keep_nodes()` | Marks nodes to keep |
| `projects/projects_libs/libfdtgen/fdtgen.c` | `fdtgen_keep_node_subtree()` | Marks node AND children |
| `projects/projects_libs/libfdtgen/fdtgen.c` | `trim_tree()` | Deletes unmarked nodes |

## Root Cause Analysis

### Code Flow Summary

The DTB generation happens in `main.c:vm_dtb_init()`:

1. **plat_keep_devices** (vmlinux.h) → `fdtgen_keep_nodes()` (NO children)
2. **plat_keep_device_and_subtree** (vmlinux.h) → `fdtgen_keep_node_subtree()` (**WITH children**)
3. **dtb([...])** paths (devices.camkes) → `fdtgen_keep_nodes()` (NO children)
4. **fdtgen_generate()** → builds final DTB, `trim_tree()` deletes unmarked nodes

### Key Insight: `/bus@0` is in `plat_keep_device_and_subtree`

Looking at `plat_include/orinagx/plat/vmlinux.h`:
```c
static const char *plat_keep_device_and_subtree[] = {
    "/bus@0",                   /* simple-bus parent - required for child probing */
    "/bus@0/misc@100000",       /* APB MISC */
    ...
};
```

Since `/bus@0` is processed by `fdtgen_keep_node_subtree()`, ALL descendants should be kept:
- `/bus@0/ethernet@6800000`
- `/bus@0/ethernet@6800000/stmmac-axi-config`
- `/bus@0/ethernet@6800000/mdio`

### RESOLVED: DTB Passthrough Works Correctly ✓ (2026-01-05)

**Verification test `20260105-165844`** (clean build, no debug code):

All kernel DTB properties reach the VM:
- `nvidia,ptp-rx-queue`, `nvidia,mtl-queues`, `nvidia,num-mtl-queues` ✓
- `stmmac-axi-config` child node ✓
- All properties defined in `kernel/tools/dts/orinagx.dts` ✓

**Key findings:**
1. `/reserved-memory` and `/firmware` do NOT exist in bootinfo FDT (expected - not in kernel DTS)
2. `/bus@0` and ALL descendants ARE kept correctly via `plat_keep_device_and_subtree`
3. Properties are passed through without modification

**What fdt_plat_customize() adds (NOT from kernel DTB):**
- `/chosen/nvidia,ether-mac0` - MAC address for nvethernet driver
- `nvidia,vm-irq-config` - VM-specific interrupt routing
- `local-mac-address` property on ethernet node

Without fdt_plat_customize(), driver fails early:
```
ether_get_mac_address_dtb: bad mac address at /chosen/nvidia,ether-mac0: NULL.
nvethernet: probe of 6800000.ethernet failed with error -2
```

### 1. `dtb()` macro uses `fdtgen_keep_nodes()`, NOT `fdtgen_keep_node_subtree()`

In `main.c:893-898`:
```c
int num_paths = 0;
char **paths = NULL;
if (camkes_dtb_get_node_paths) {
    paths = camkes_dtb_get_node_paths(&num_paths);
}
fdtgen_keep_nodes(context, (const char **)paths, num_paths);
```

The paths from `dtb([...])` in devices.camkes go to `fdtgen_keep_nodes()`.

### 2. Difference between `keep_nodes` and `keep_node_subtree`

| Function | Keeps Node | Keeps Properties | Keeps Children |
|----------|------------|------------------|----------------|
| `fdtgen_keep_nodes()` | Yes | Yes | **NO** |
| `fdtgen_keep_node_subtree()` | Yes | Yes | **YES** |

### 3. `trim_tree()` deletes unmarked nodes

```c
// fdtgen.c:331-372
static void trim_tree(fdtgen_context_t *handle, int offset)
{
    fdt_for_each_subnode(child, dtb, offset) {
        // ...
        HASH_FIND_STR(handle->nodes_table, handle->string_buf, this);
        if (this == NULL) {
            int err = fdt_del_node(dtb, child);  // Deletes entire child node!
            // ...
        }
    }
}
```

This explains why `stmmac-axi-config` (a child of ethernet) is deleted - it's not in the keep list.

**BUT**: If `/bus@0` is in `plat_keep_device_and_subtree`, children SHOULD be in keep list!

### 4. Properties Mystery - RESOLVED ✓

Properties of kept nodes ARE preserved by fdtgen. The issue was **cache coherency** when loading the generated DTB to VM RAM.

#### Investigation Timeline (2026-01-05)

**Step 1: Buffer Overflow Fix (PARTIAL)**

Initial symptom: properties added by `fdt_plat_customize()` not visible.

```
fdt_plat_customize@fdt.c:541 Expanded FDT from 327680 to 458752 bytes
```

- `DTB_BUFFER_SIZE` in main.c was 320KB (327680 bytes)
- `fdt_open_into()` tried to expand beyond buffer size
- **Fix:** Increased `DTB_BUFFER_SIZE` to 512KB

**Step 2: Still Broken After Buffer Fix**

After fixing buffer size, properties STILL missing:
```
Expanded FDT from 524288 to 524288 bytes  <-- now 512KB, no overflow
Added MGBE properties (ptp-rx-queue=0, ...)
nvidia,ptp-rx-queue: NOT FOUND  <-- STILL not visible in VM!
```

**Step 3: Added Debug Verification**

Added code to verify DTB content at each stage:
- PRE-CUSTOMIZE: Check if properties exist in bootinfo FDT
- POST-PACK: Verify properties after `fdt_pack()` - **ALL PROPERTIES FOUND!**
- VM-DTB-READBACK: Read back DTB from VM RAM after `vm_ram_touch()` - **CORRUPTED!**

Debug output showed:
```
POST-PACK: fdt_totalsize=7723, magic=0xd00dfeed  <-- VMM sees correct DTB
VM-DTB-READBACK: magic=0x00000191, size=1920478569  <-- Guest sees garbage!
```

**Step 4: ROOT CAUSE IDENTIFIED - Cache Coherency**

The `load_generated_dtb()` callback in main.c did NOT flush cache after writing:

```c
// BEFORE (broken):
static int load_generated_dtb(vm_t *vm, uintptr_t paddr, void *addr, ...)
{
    memcpy(addr, cookie + offset, size);
    return 0;  // <-- NO CACHE FLUSH!
}
```

Guest image loading (`guest_image.c`) flushes cache after each page:
```c
if (vm->mem.clean_cache) {
    seL4_CPtr cap = vspace_get_cap(&vm->mem.vmm_vspace, vaddr);
    seL4_ARM_Page_CleanInvalidate_Data(cap, 0, PAGE_SIZE_4K);
}
```

But `load_generated_dtb()` was missing this pattern entirely.

**Step 5: FIX APPLIED**

Added cache flush to `load_generated_dtb()`:

```c
// AFTER (fixed):
static int load_generated_dtb(vm_t *vm, uintptr_t paddr, void *addr, ...)
{
    memcpy(addr, cookie + offset, size);

    if (vm->mem.clean_cache) {
        seL4_CPtr cap = vspace_get_cap(&vm->mem.vmm_vspace, addr);
        seL4_ARM_Page_CleanInvalidate_Data(cap, 0, PAGE_SIZE_4K);
    }
    return 0;
}
```

**Why This Happens**

`vm_ram_touch()` uses a **sliding window mapping** - the same VMM virtual address maps to different guest physical pages sequentially. The VMM writes DTB data to this address, but:

1. Data stays in CPU cache (not flushed to DRAM)
2. Guest VM has its own non-coherent mapping of the same physical memory
3. Guest reads from DRAM, sees stale/uninitialized data

This is the same pattern as the Tegra cache coherency issues documented in `tegra-cache-operations.md`, but happening in the VMM layer rather than the kernel.

## Platform-Specific Keep Lists

The `plat_keep_device_and_subtree` array in `vmlinux.h` uses `fdtgen_keep_node_subtree()`:

```c
// plat_include/orinagx/plat/vmlinux.h
static const char *plat_keep_device_and_subtree[] = {
    "/bus@0",                   /* simple-bus parent */
    "/bus@0/misc@100000",       /* APB MISC */
    "/bus@0/serial@31d0000",    /* UARTI */
    "/bus@0/hsp@c150000",       /* HSP AON */
    "/bus@0/hsp@3c00000",       /* HSP Top0 */
    "/sram@40000000",           /* CPU-BPMP shared memory */
    "/bpmp",                    /* BPMP */
    "/reserved-memory",
    "/firmware",
};
```

But `dtb()` paths from devices.camkes use `fdtgen_keep_nodes()` - not subtree!

## Required: fdt_plat_customize()

`fdt_plat_customize()` adds VM-specific properties that are NOT in the kernel DTB:

```c
// projects/vm/components/VM_Arm/src/modules/plat/orinagx/fdt.c
int fdt_plat_customize(void *fdt)
{
    // Required for nvethernet driver:
    fdt_add_mgbe_mac_address(fdt);     // /chosen/nvidia,ether-mac0
    fdt_add_mgbe_vm_irq_config(fdt);   // nvidia,vm-irq-config property

    // Optional: override/add properties not in kernel DTB
    fdt_add_mgbe_properties(fdt);      // Only needed if kernel DTB is incomplete
}
```

**Note:** Kernel DTB properties (nvidia,ptp-rx-queue, etc.) pass through correctly.
Only VM-specific properties (MAC address, vm-irq-config) need to be added here.

## Proposed Solutions

### Solution 1: Add `subtree` option to `dtb()` macro

Modify CAmkES to support:
```camkes
vm0.dtb = dtb([
    {"path": "/bus@0/ethernet@6800000", "subtree": true},
    {"path": "/bus@0/gpio@2200000", "subtree": true},
]);
```

Implementation in `main.c`:
```c
if (subtree_flag) {
    fdtgen_keep_node_subtree(context, fdt_ori, path);
} else {
    fdtgen_keep_nodes(context, &path, 1);
}
```

### Solution 2: DTB overlay mechanism

Allow importing subtrees from external DTBs:

```camkes
vm0.dtb = dtb([
    {"path": "/bus@0/ethernet@6800000"},
]);
vm0.dtb_overlay = "/path/to/tegra234-ethernet.dtbo";
```

### Solution 3: Reference DTB passthrough

Use the bitbake-generated Linux DTB as a reference and copy entire subtrees:

```c
// In fdt_plat_customize()
int fdt_copy_subtree_from_reference(void *dst_fdt, void *ref_fdt, const char *path);
```

This would allow using the full Linux DTS (from `meta-tegra`) without duplication.

### Solution 4: Move dtb() paths to plat_keep_device_and_subtree

Instead of using `dtb()` in devices.camkes, add paths to `vmlinux.h`:

```c
static const char *plat_keep_device_and_subtree[] = {
    // ... existing entries ...
    "/bus@0/ethernet@6800000",  // Add here instead of dtb()
    "/bus@0/gpio@2200000",
};
```

**Pros:** Uses existing `fdtgen_keep_node_subtree()` mechanism
**Cons:** Platform-specific, not configurable per-application

## Verification

To verify properties are reaching the VM, add debug output in init script:

```bash
ETHDT="/proc/device-tree/bus@0/ethernet@6800000"
if [ -d "$ETHDT" ]; then
    echo "nvidia,ptp-rx-queue: $(xxd -p $ETHDT/nvidia,ptp-rx-queue 2>/dev/null || echo 'NOT FOUND')"
    echo "nvidia,mtl-queues: $(xxd -p $ETHDT/nvidia,mtl-queues 2>/dev/null || echo 'NOT FOUND')"
fi
```

## References

- `projects/projects_libs/libfdtgen/fdtgen.c` - fdtgen implementation
- `projects/vm/components/VM_Arm/src/main.c` - DTB generation in VM
- `projects/camkes-tool/libsel4camkes/src/io.c` - CAmkES FDT interface
- `projects/vm/components/VM_Arm/plat_include/orinagx/plat/vmlinux.h` - Platform keep lists
- `kernel/tools/dts/orinagx.dts` - Source device tree
