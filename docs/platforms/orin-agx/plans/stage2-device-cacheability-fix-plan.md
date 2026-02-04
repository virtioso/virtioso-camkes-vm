# Stage-2 Device Memory Cacheability Fix Plan

## Problem Summary

Device passthrough memory is mapped with `cacheable=1` (S2_NORMAL) when it should use `cacheable=0` (S2_DEVICE_nGnRnE). See `stage2-device-cacheability-bug.md` for full analysis.

## Option A: Conservative - Default DTB Passthrough to Non-Cacheable

### Approach

Check if an address is in a DTB device range before creating the reservation, and use `cacheable=0` for device addresses.

### Changes Required

| File | Change |
|------|--------|
| `VM_Arm/src/main.c` | Add `is_address_in_dtb_device_range()` function |
| `VM_Arm/src/main.c` | Modify `unhandled_mem_fault_callback()` to check device vs RAM |
| `libsel4vm/src/guest_memory.c` | Add `vm_reserve_memory_at_uncached()` or add cacheable param |
| `libsel4vm/include/sel4vm/guest_memory.h` | Expose new API |

### Complexity: Medium

**Pros:**
- Simple conceptual model (device = uncached, RAM = cached)
- No changes to `vm_frame_t` structure
- No changes to vspace layer

**Cons:**
- Requires querying DTB device ranges at fault time
- Two reservation functions (or optional parameter)
- Doesn't extend to non-DTB device memory elegantly

### Implementation Sketch

```c
// In main.c
static bool is_address_in_dtb_device_range(uintptr_t addr) {
    if (!camkes_dtb_untyped_count) return false;
    int cnt = camkes_dtb_untyped_count();
    for (int i = 0; i < cnt; i++) {
        size_t size_bits;
        uintptr_t paddr;
        camkes_dtb_get_nth_untyped(i, &size_bits, &paddr);
        if (addr >= paddr && addr < paddr + BIT(size_bits))
            return true;
    }
    return false;
}

memory_fault_result_t unhandled_mem_fault_callback(...) {
    uintptr_t addr = PAGE_ALIGN(paddr, SIZE_BITS_TO_BYTES(seL4_PageBits));

    // Check if device memory (needs non-cacheable mapping)
    bool is_device = is_address_in_dtb_device_range(addr);

    // Use appropriate reservation function
    if (is_device) {
        reservation = vm_reserve_device_memory_at(vm, addr, SIZE_BITS_TO_BYTES(seL4_PageBits),
                                                   handle_on_demand_fault_callback, NULL);
    } else {
        reservation = vm_reserve_memory_at(vm, addr, SIZE_BITS_TO_BYTES(seL4_PageBits),
                                           handle_on_demand_fault_callback, NULL);
    }
    ...
}
```

---

## Option B: Add Cacheable Field to vm_frame_t

### Approach

Add a `cacheable` field to `vm_frame_t`, set it in the iterator based on device vs RAM, and use it during mapping.

### Changes Required

| File | Change | Complexity |
|------|--------|------------|
| `libsel4vm/include/sel4vm/guest_memory.h` | Add `int cacheable` to `vm_frame_t` | Trivial |
| `VM_Arm/src/main.c` | Set `frame_result.cacheable` in `alloc_vm_device_cap()` and `alloc_vm_ram_cap()` | Trivial |
| `libsel4vm/src/guest_memory.c` | Use `frame.cacheable` instead of reservation's cacheable | **Complex** |
| `libsel4vm/src/guest_vspace.c` | May need new mapping path | Medium |
| Potentially `seL4_libs` vspace | If we need per-page cacheable | High |

### The Core Challenge

The mapping happens in `map_vm_memory_reservation()` at line 540-542:
```c
int ret = vspace_deferred_rights_map_pages_at_vaddr(&vm->mem.vm_vspace,
            &reservation_frame.cptr, NULL,
            (void *)reservation_frame.vaddr, 1, reservation_frame.size_bits,
            reservation_frame.rights, vm_reservation->vspace_reservation);
```

This uses `vm_reservation->vspace_reservation` which has the cacheable attribute baked in from creation time. The `vspace_deferred_rights_map_pages_at_vaddr()` function eventually calls:
```c
return map_pages_at_vaddr(vspace, caps, cookies, vaddr, num_pages, size_bits,
                          rights, res->cacheable);  // Uses reservation's cacheable!
```

### Three Sub-Options for Option B

#### B1: Modify reservation cacheable per-frame (Hacky)

Update `res->cacheable` before mapping each frame:
```c
// In map_vm_memory_reservation()
sel4utils_res_t *res = reservation_to_res(vm_reservation->vspace_reservation);
res->cacheable = reservation_frame.cacheable;  // Modify in place!
int ret = vspace_deferred_rights_map_pages_at_vaddr(...);
```

**Problem:** The reservation is shared, so this would affect other mappings. Also violates encapsulation.

#### B2: Create per-frame reservations (Wasteful)

Create a new reservation for each frame with the correct cacheable:
```c
// In map_vm_memory_reservation()
reservation_t frame_res = vspace_reserve_range_at(&vm->mem.vm_vspace,
                            (void *)reservation_frame.vaddr,
                            BIT(reservation_frame.size_bits),
                            seL4_AllRights,
                            reservation_frame.cacheable);  // Correct cacheable
int ret = vspace_map_pages_at_vaddr(..., frame_res);
vspace_free_reservation(&vm->mem.vm_vspace, frame_res);
```

**Problem:** Wasteful allocation/deallocation for each page.

#### B3: Bypass reservation system for direct mapping (Cleanest)

Add a new function that maps directly without using the reservation's cacheable:
```c
// In guest_vspace.c - new function
int vm_map_frame_direct(vm_t *vm, seL4_CPtr cap, uintptr_t vaddr,
                        seL4_CapRights_t rights, int cacheable, size_t size_bits)
{
    // Call guest_vspace_map_page_arch directly
    return guest_vspace_map_page_arch(&vm->mem.vm_vspace, cap, vaddr,
                                       rights, cacheable, size_bits);
}
```

Then in `map_vm_memory_reservation()`:
```c
// Instead of vspace_deferred_rights_map_pages_at_vaddr()
int ret = vm_map_frame_direct(vm, reservation_frame.cptr, reservation_frame.vaddr,
                              reservation_frame.rights, reservation_frame.cacheable,
                              reservation_frame.size_bits);
```

**This requires:**
1. Exposing `guest_vspace_map_page_arch` or creating a wrapper
2. Updating `map_vm_memory_reservation()` to use direct mapping
3. Handling IOMMU mappings if CONFIG_TK1_SMMU/CONFIG_IOMMU is enabled

### Complexity: Medium-High

**Pros:**
- Clean conceptual model (frame knows its own cacheability)
- Extends naturally to any memory type
- No need to query device ranges at fault time

**Cons:**
- More invasive changes to the mapping path
- Need to handle IOMMU case
- Changes public API (`vm_frame_t`)

---

## Comparison

| Aspect | Option A | Option B (B3) |
|--------|----------|---------------|
| Lines of code | ~50 | ~80 |
| Files modified | 3-4 | 4-5 |
| API changes | Add `vm_reserve_device_memory_at()` | Modify `vm_frame_t` |
| Conceptual model | "Check address, pick function" | "Frame carries its metadata" |
| Future extensibility | Add more check functions | Natural |
| Risk of breaking existing code | Low | Medium |
| IOMMU handling | N/A | Need to update |

## Recommendation

**Option B (sub-option B3)** is the cleaner design, but requires more careful implementation due to the IOMMU case and the need to expose/create a direct mapping function.

**Option A** is simpler to implement quickly and carries less risk, but is less elegant.

For **immediate fix**, Option A is faster. For **proper fix**, Option B is better.

---

## Detailed Option B3 Implementation Plan

### Step 1: Modify vm_frame_t

```c
// libsel4vm/include/sel4vm/guest_memory.h
typedef struct vm_frame {
    seL4_CPtr cptr;
    seL4_CapRights_t rights;
    uintptr_t vaddr;
    size_t size_bits;
    int cacheable;  // NEW: 0=device, 1=cacheable RAM
} vm_frame_t;
```

### Step 2: Update frame allocation functions

```c
// VM_Arm/src/main.c
static int alloc_vm_device_cap(uintptr_t addr, vm_t *vm, vm_frame_t *frame_result)
{
    // ... existing allocation code ...
    frame_result->cptr = frame.capPtr;
    frame_result->rights = seL4_AllRights;
    frame_result->vaddr = addr;
    frame_result->size_bits = seL4_PageBits;
    frame_result->cacheable = 0;  // Device = non-cacheable
    return 0;
}

static int alloc_vm_ram_cap(uintptr_t addr, vm_t *vm, vm_frame_t *frame_result)
{
    // ... existing allocation code ...
    frame_result->cptr = frame.capPtr;
    frame_result->rights = seL4_AllRights;
    frame_result->vaddr = addr;
    frame_result->size_bits = seL4_PageBits;
    frame_result->cacheable = 1;  // RAM = cacheable
    return 0;
}
```

### Step 3: Add direct mapping function

```c
// libsel4vm/src/guest_memory.c or guest_vspace.c
#include "guest_vspace_arch.h"

static int vm_map_frame_direct(vm_t *vm, vm_frame_t *frame)
{
    vspace_t *vspace = &vm->mem.vm_vspace;

    // Direct mapping with explicit cacheable
    int error = guest_vspace_map_page_arch(vspace, frame->cptr,
                                           (void *)frame->vaddr,
                                           frame->rights, frame->cacheable,
                                           frame->size_bits);
    if (error) {
        return error;
    }

    // Update vspace tracking (needed for unmap)
    error = update_entries(/* ... */);

    return error;
}
```

### Step 4: Update map_vm_memory_reservation

```c
// libsel4vm/src/guest_memory.c
int map_vm_memory_reservation(...)
{
    while (bytes_left) {
        vm_frame_t reservation_frame = map_iterator(current_addr, map_cookie);
        // ... validation ...

        // Use direct mapping with frame's cacheable
        int ret = vm_map_frame_direct(vm, &reservation_frame);
        if (ret) {
            ZF_LOGE("Failed to map address 0x%"PRIxPTR, reservation_frame.vaddr);
            break;
        }
        // ... continue ...
    }
}
```

### Step 5: Handle IOMMU case

If `CONFIG_TK1_SMMU` or `CONFIG_IOMMU` is defined, the guest_vspace_map function also maps into IOMMUs. We need to ensure the direct mapping does the same.

### Step 6: Update all vm_frame_t initializations

Search for all places that create `vm_frame_t` and ensure they set `.cacheable`:
- `on_demand_iterator()`
- `frames_map_memory_iterator()`
- Any other iterators in the codebase

---

## Files to Modify Summary (Option B3)

1. `libsel4vm/include/sel4vm/guest_memory.h` - Add cacheable to vm_frame_t
2. `libsel4vm/src/guest_memory.c` - Add vm_map_frame_direct(), update map_vm_memory_reservation()
3. `libsel4vm/src/guest_vspace.c` - May need to expose guest_vspace_map or similar
4. `VM_Arm/src/main.c` - Set cacheable in alloc_vm_device_cap(), alloc_vm_ram_cap()
5. `VM_Arm/src/modules/map_frame_hack.c` - Update vm_frame_t initialization

## Testing

1. Build vm_minimal for Orin AGX
2. Boot and verify BPMP probe succeeds
3. Check guest can access clocks/resets via BPMP
4. Run existing vm_qemu_virtio tests on RPi4 to ensure no regression
