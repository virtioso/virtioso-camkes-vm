# Plan: DTB Generation Enhancements for CAmkES VM

## Problem Statement

CAmkES's current DTB generation extracts nodes individually from the platform DTB using `fdtgen`. When a node references another node via phandle (e.g., TCU's `mboxes` referencing HSP AON), the phandle values can become invalid because:

1. Phandles are assigned during DTB compilation
2. When `fdtgen` extracts nodes individually, it preserves the original phandle values
3. If both referencing and referenced nodes aren't extracted together, or if a provided DTB has different phandles, references break

**Example**: Orin AGX TCU console
- TCU node has `mboxes = <&hsp_aon 0 0x13000001>`
- HSP AON node has `phandle = <0x141>` (in generated DTB)
- If TCU is in a provided DTB with `phandle = <0x01>`, reference breaks

## Current Architecture (Upstream)

```
projects/vm/components/VM_Arm/src/main.c:
  generate_dtb()
    ├── fdtgen_new_context(gen_dtb_buf)
    ├── fdtgen_keep_nodes(...)      # Add nodes to keep list
    ├── fdtgen_generate(context, fdt_ori)  # Extract nodes from platform DTB
    ├── fdt_generate_memory_node()  # Add memory node
    └── return (DTB in gen_dtb_buf)

  load_vm_images()
    ├── vm_load_guest_kernel()
    ├── fdt_generate_chosen_node()  # Add chosen node
    ├── fdt_append_chosen_node_with_initrd_info()
    ├── fdt_pack(gen_dtb_buf)       # Pack DTB
    └── vm_ram_touch(...)           # Load DTB to guest
```

## Current Architecture (Virtioso virtioso-camkes-vm)

Virtioso adds a post-generation hook via the existing `DEFINE_MODULE` infrastructure:

```c
// src/camkes/modules/fdt_plat_customize.c
int WEAK fdt_plat_customize(vm_t *vm, void *dtb_buf) { return 0; }

static void fdt_plat_customize_init(vm_t *vm, void *cookie) {
    fdt_plat_customize(vm, cookie);  // cookie = gen_dtb_buf
}

DEFINE_MODULE(fdt_plat_customize, gen_dtb_buf, fdt_plat_customize_init)

// src/plat/rpi4/fdt.c - Platform override
int fdt_plat_customize(vm_t *vm, void *dtb_buf) {
    fdt_generate_usb_node(dtb_buf);  // Add USB node programmatically
    return 0;
}
```

**Key insight**: Virtioso's approach uses `gen_dtb_buf` as module cookie, so the hook receives the generated DTB buffer and can modify it.

---

## Proposed Solutions

### A) DTB Overlay Support

Add ability to apply DTB overlays after base DTB generation.

#### A.1 New CAmkES Configuration Options

```c
// devices.camkes
vm0.vm_image_config = {
    "generate_dtb" : true,
    "dtb_overlays" : ["console-tcu.dtbo", "virtio-devices.dtbo"],  // NEW
    ...
};
```

#### A.2 Changes to projects/vm

**File: `components/VM_Arm/configurations/vm.h`**
```c
// Add to vm_image_config_t or as separate attribute
string dtb_overlays[];
```

**File: `templates/seL4VMParameters.template.c`**
```c
// Generate overlay file list
static const char *vm_dtb_overlays[] = {
/*- for overlay in me.dtb_overlays -*/
    "/*? overlay ?*/",
/*- endfor -*/
};
static const int vm_num_dtb_overlays = /*? len(me.dtb_overlays) ?*/;
```

**File: `components/VM_Arm/src/main.c`**

Add after `fdtgen_generate()` and before `fdt_generate_memory_node()`:

```c
// Apply DTB overlays
for (int i = 0; i < vm_num_dtb_overlays; i++) {
    const char *overlay_name = vm_dtb_overlays[i];
    void *overlay_data;
    size_t overlay_size;

    // Load overlay from file server
    err = load_file_from_cpio(overlay_name, &overlay_data, &overlay_size);
    if (err) {
        ZF_LOGE("Failed to load DTB overlay: %s", overlay_name);
        return -1;
    }

    // Apply overlay using libfdt
    err = fdt_overlay_apply(gen_dtb_buf, overlay_data);
    if (err) {
        ZF_LOGE("Failed to apply DTB overlay %s: %d", overlay_name, err);
        return -1;
    }

    ZF_LOGI("Applied DTB overlay: %s", overlay_name);
}
```

#### A.3 Build System Changes

**File: `components/VM_Arm/CMakeLists.txt` or app CMakeLists.txt**

```cmake
# Compile DTB overlays from .dtso files
foreach(overlay ${VM_DTB_OVERLAYS})
    get_filename_component(overlay_name ${overlay} NAME_WE)
    add_custom_command(
        OUTPUT ${overlay_name}.dtbo
        COMMAND dtc -@ -I dts -O dtb -o ${overlay_name}.dtbo ${overlay}
        DEPENDS ${overlay}
        COMMENT "Compiling DTB overlay ${overlay_name}"
    )
    list(APPEND DTB_OVERLAY_TARGETS ${overlay_name}.dtbo)
endforeach()

# Add overlays to file server
AddToFileServer("console-tcu.dtbo" "${CMAKE_CURRENT_BINARY_DIR}/console-tcu.dtbo")
```

#### A.4 Example Overlay for Orin AGX TCU

```dts
// orinagx/console-tcu.dtso
/dts-v1/;
/plugin/;

&{/} {
    serial-guest {
        compatible = "nvidia,tegra234-tcu", "nvidia,tegra194-tcu";
        mboxes = <&{/bus@0/hsp@c150000} 0 0x13000001>;
        mbox-names = "tx";
        status = "okay";
    };
};
```

**Key**: The `&{/bus@0/hsp@c150000}` syntax resolves the phandle at overlay-apply time against the base DTB.

---

### B) Programmatic Post-Generation Hook (Upstream-Compatible)

Upstream the Virtioso approach with minimal changes.

#### B.1 Changes to projects/vm

**File: `components/VM_Arm/include/vmlinux.h`**

```c
// Add weak hook declaration
int fdt_plat_customize(vm_t *vm, void *dtb_buf) WEAK;
```

**File: `components/VM_Arm/src/main.c`**

Add after DTB generation, before `fdt_pack()`:

```c
// In load_vm_images(), after chosen/initrd nodes added:
if (vm_config->generate_dtb) {
    // ... existing code ...

    // Platform-specific DTB customization hook
    if (fdt_plat_customize) {
        err = fdt_plat_customize(vm, gen_dtb_buf);
        if (err) {
            ZF_LOGE("fdt_plat_customize() failed (%d)", err);
            return -1;
        }
    }

    fdt_pack(gen_dtb_buf);
    // ... rest of code ...
}
```

**File: `components/VM_Arm/src/fdt_plat_customize.c` (new file)**

```c
/*
 * Copyright 2024, seL4 Project
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Platform-specific DTB customization hook.
 * Override in platform code to add/modify DTB nodes after generation.
 */

#include <vmlinux.h>

int WEAK fdt_plat_customize(vm_t *vm, void *dtb_buf)
{
    /* Default: no customization */
    return 0;
}
```

#### B.2 Platform Override Example

**File: `components/VM_Arm/plat/orinagx/fdt_customize.c`**

```c
#include <libfdt.h>
#include <vmlinux.h>

int fdt_plat_customize(vm_t *vm, void *dtb_buf)
{
    int err;

    // Find HSP AON phandle
    int hsp_off = fdt_path_offset(dtb_buf, "/bus@0/hsp@c150000");
    if (hsp_off < 0) {
        ZF_LOGE("HSP AON node not found");
        return -1;
    }
    uint32_t hsp_phandle = fdt_get_phandle(dtb_buf, hsp_off);
    if (!hsp_phandle) {
        // Assign a phandle if not present
        hsp_phandle = fdt_get_max_phandle(dtb_buf) + 1;
        fdt_setprop_u32(dtb_buf, hsp_off, "phandle", hsp_phandle);
    }

    // Add TCU node with correct phandle reference
    int root = fdt_path_offset(dtb_buf, "/");
    int tcu = fdt_add_subnode(dtb_buf, root, "serial-guest");
    if (tcu < 0) {
        ZF_LOGE("Failed to add serial-guest node");
        return -1;
    }

    fdt_setprop_string(dtb_buf, tcu, "compatible",
                       "nvidia,tegra234-tcu\0nvidia,tegra194-tcu");

    // mboxes: <phandle type param>
    uint32_t mboxes[3] = {
        cpu_to_fdt32(hsp_phandle),
        cpu_to_fdt32(0),
        cpu_to_fdt32(0x13000001)
    };
    fdt_setprop(dtb_buf, tcu, "mboxes", mboxes, sizeof(mboxes));
    fdt_setprop_string(dtb_buf, tcu, "mbox-names", "tx");
    fdt_setprop_string(dtb_buf, tcu, "status", "okay");

    ZF_LOGI("Added serial-guest node with HSP AON phandle 0x%x", hsp_phandle);
    return 0;
}
```

---

## What Becomes Redundant in virtioso-camkes-vm

With upstream adoption of either approach:

| Virtioso File | Status | Notes |
|----------|--------|-------|
| `src/camkes/modules/fdt_plat_customize.c` | **REDUNDANT** | Module wrapper no longer needed |
| `src/plat/rpi4/fdt.c` | **KEEP** (move) | Move to `projects/vm/plat/rpi4/` |
| `src/fdt.c` | **PARTIAL** | Some helpers useful, others duplicate libfdt |
| `include/virtioso/fdt.h` | **PARTIAL** | `fdt_plat_customize` declaration moves upstream |

### Specific Functions

**Redundant (in upstream):**
- `DEFINE_MODULE(fdt_plat_customize, ...)` wrapper
- `fdt_plat_customize_init()` module init

**Keep in virtioso-camkes-vm (Virtioso-specific features):**
- `fdt_node_t` infrastructure for dynamic node registration
- `DEFINE_FDT_NODE` macro
- `fdt_generate_reserved_node()` - for shared memory regions
- `fdt_generate_pci_node()` - for PCI passthrough
- SWIOTLB node generation

---

## Recommended Implementation Order

1. **Phase 1: Upstream Post-Generation Hook** (Option B)
   - Minimal upstream changes
   - Single weak function + default implementation
   - Platforms override as needed
   - Virtioso removes module wrapper, keeps platform overrides

2. **Phase 2: DTB Overlay Support** (Option A)
   - More powerful but more complex
   - Requires build system changes
   - Good for complex device configurations
   - Can coexist with programmatic hook

---

## Layered Implementation Strategy

### The DEFINE_MODULE_DEP Challenge

Virtioso's current implementation uses `DEFINE_MODULE` with `DEFINE_MODULE_DEP` to ensure
`fdt_plat_customize` runs **after** other Virtioso modules that also modify the DTB. A simple
weak function call in upstream `main.c` wouldn't preserve this inter-module ordering.

### Recommended Layered Approach

**Layer 1: Upstream (projects/vm)** - Minimal change at commit ad66b65 or later

```c
// components/VM_Arm/src/main.c - in load_vm_images(), after chosen/initrd

int WEAK fdt_plat_customize(vm_t *vm, void *dtb_buf) { return 0; }

// ... in load_vm_images():
if (vm_config->generate_dtb) {
    // ... existing chosen/initrd code ...

    // Platform-specific DTB customization hook
    err = fdt_plat_customize(vm, gen_dtb_buf);
    if (err) {
        ZF_LOGE("fdt_plat_customize() failed (%d)", err);
        return -1;
    }

    fdt_pack(gen_dtb_buf);
}
```

This is ~10 lines added to upstream. Clean, minimal, no module system involvement.

**Layer 2: Virtioso (virtioso-camkes-vm)** - Thin wrapper for module ordering

```c
// src/camkes/modules/fdt_plat_customize.c
// This wrapper ensures fdt_plat_customize runs after other Virtioso modules

static int fdt_plat_customize_impl(vm_t *vm, void *dtb_buf);

static void fdt_plat_customize_module_init(vm_t *vm, void *cookie)
{
    if (!vm_config.generate_dtb) {
        return;
    }
    // Store for later - actual call happens via upstream weak function
    // Or call impl directly if we override the weak function
}

DEFINE_MODULE(fdt_plat_customize, gen_dtb_buf, fdt_plat_customize_module_init)
DEFINE_MODULE_DEP(fdt_plat_customize, some_other_virtioso_module)

// Override upstream weak function
int fdt_plat_customize(vm_t *vm, void *dtb_buf)
{
    return fdt_plat_customize_impl(vm, dtb_buf);
}
```

**Layer 3: Platform (virtioso-camkes-vm/src/plat/orinagx/)** - Actual customization

```c
// src/plat/orinagx/fdt.c
static int fdt_plat_customize_impl(vm_t *vm, void *dtb_buf)
{
    // Add TCU node with correct HSP AON phandle reference
    return orinagx_add_tcu_node(dtb_buf);
}
```

### Benefits of This Layered Approach

1. **Upstream stays minimal**: Just a weak function call, no module system changes
2. **Virtioso keeps ordering control**: `DEFINE_MODULE_DEP` ensures proper sequencing
3. **Clean rebase**: Virtioso changes layer on top of upstream without conflicts
4. **Platforms can be simple**: If no ordering needed, just override weak function directly

### Git History Structure

```
projects/vm (upstream):
  ad66b65  ... existing commit ...
  NEW      Add fdt_plat_customize weak hook for platform DTB customization

projects/virtioso-camkes-vm (Virtioso fork):
  ... existing Virtioso commits ...
  KEEP     Module wrapper with DEFINE_MODULE_DEP (rebases cleanly on upstream hook)
  NEW      Add orinagx/fdt.c with TCU node generation
```

---

## Impact on Orin AGX TCU Issue

With **Option B** (programmatic hook):
1. CAmkES generates DTB with HSP AON extracted (has phandle 0x141)
2. Platform hook runs, finds HSP AON's actual phandle
3. Hook adds serial-guest node with correct mboxes reference
4. TCU driver finds mailbox provider correctly

With **Option A** (DTB overlay):
1. CAmkES generates base DTB
2. Overlay applied: `&{/bus@0/hsp@c150000}` resolves to phandle 0x141
3. serial-guest node added with correct reference
4. TCU driver works

Both solve the phandle mismatch problem. Option B is simpler and can be implemented immediately.
