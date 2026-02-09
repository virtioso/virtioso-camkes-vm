# kmod-sel4-virt Backend-Side Analysis

This document is a detailed backend-side analysis of `sources/kmod-sel4-virt`, focused on kernel control/data paths, RPC behavior, lifecycle semantics, and risk areas.

## Scope

- Repository path analyzed: `sources/kmod-sel4-virt`
- Primary focus: backend-side guest kernel module behavior (core + PCI backend + RPC + mmap + irqfd/ioeventfd)
- Build artifact context: `sel4_virt` module (`obj-m := sel4_virt.o`)

Key references:
- `sources/kmod-sel4-virt/Makefile:17`
- `sources/kmod-sel4-virt/Makefile:18`

## Component Structure

### Core control plane

- VM lifecycle and `/dev/sel4` char interface:
  - `sources/kmod-sel4-virt/sel4_core.c:106`
  - `sources/kmod-sel4-virt/sel4_core.c:277`
  - `sources/kmod-sel4-virt/sel4_core.c:376`
- Internal driver contracts/types:
  - `sources/kmod-sel4-virt/sel4_virt_drv.h:28`
  - `sources/kmod-sel4-virt/sel4_virt_drv.h:54`
  - `sources/kmod-sel4-virt/sel4_virt_drv.h:69`

### PCI backend binding

- Dataport discovery and VMM construction from PCI BARs:
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:245`
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:180`
- Dataport lifecycle and hot-remove handling:
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:373`
  - `sources/kmod-sel4-virt/sel4_core.c:437`
- Pooling/matching VMM objects:
  - `sources/kmod-sel4-virt/pci/sel4_vmm_pool.c:34`
  - `sources/kmod-sel4-virt/pci/sel4_vmm_pool.c:85`

### RPC transport

- UAPI RPC envelope and operation definitions:
  - `sources/kmod-sel4-virt/include/uapi/sel4/rpc.h:79`
  - `sources/kmod-sel4-virt/include/uapi/sel4/rpc.h:93`
- Lock-free queue/message ownership model:
  - `sources/kmod-sel4-virt/include/uapi/sel4/rpc_queue.h:137`
  - `sources/kmod-sel4-virt/include/uapi/sel4/rpc_queue.h:442`

### Fast-path acceleration and memory export

- MMIO write fast path via ioeventfd:
  - `sources/kmod-sel4-virt/sel4_ioeventfd.c:206`
- IRQ injection path via irqfd:
  - `sources/kmod-sel4-virt/sel4_irqfd.c:40`
  - `sources/kmod-sel4-virt/sel4_irqfd.c:91`
- mmap exposure for RAM/IOBUF/EVENT BAR:
  - `sources/kmod-sel4-virt/sel4_mmap.c:103`

## Naming Rationale

This code path currently mixes multiple concepts under `vmm`/`dataport` naming.
During review, the following semantic tension was identified:

- `virtio_*` naming is too narrow:
  - The transport can carry generic MMIO/device-model traffic, not only virtio.
  - References: `sources/kmod-sel4-virt/sel4_core.c:46`, `sources/kmod-sel4-virt/include/uapi/sel4/rpc.h:79`
- `vmm` naming is too origin-specific:
  - This module should not rely on whether the other side is a guest VMM process, native seL4 thread, or another producer implementing the same shared-memory RPC contract.
- `emulation_*` naming is misleading:
  - Some flows (for example virtio transport) are not naturally described as “emulation”.
- `peer_*` implies symmetry:
  - The roles here are asymmetric in practice (`device` side vs `driver` side).
- `service_*` can suggest web/service architecture and was considered less intuitive for this kernel/hardware-adjacent path.

Recommended role-based, transport-agnostic naming:

- `device_endpoint`:
  - One discovered endpoint/dataport object.
  - Maps well to current `struct sel4_dataport` in `sources/kmod-sel4-virt/pci/sel4_pci.c`.
- `device_bundle`:
  - The paired `{iobuf, ram}` unit keyed by vmid.
  - This concept exists implicitly today in probe/pool logic but is not first-class.
  - References: `sources/kmod-sel4-virt/pci/sel4_pci.c:341`, `sources/kmod-sel4-virt/pci/sel4_vmm_pool.c:85`
- `device_context` (or `vm_device_context`):
  - Runtime object that carries RPC, mappings, IRQ, and VM binding.
  - Candidate replacement/wrapper around current `struct sel4_vmm`.
  - References: `sources/kmod-sel4-virt/sel4_virt_drv.h:54`, `sources/kmod-sel4-virt/sel4_core.c:106`

Pragmatic migration approach:

1. Introduce the missing first-class bundle type (`device_bundle`) without behavior changes.
2. Rename endpoint types/functions (`sel4_dataport` -> `device_endpoint`) in PCI backend.
3. Rename/wrap runtime object (`sel4_vmm`) only after step 1-2 to keep churn controlled.

### Additional Naming Notes (`driver`, `peer`, `service`)

Follow-up reasoning from review discussion:

- Why not use `driver_*` as primary type names?
  - `driver` is semantically valid in virtio terminology, but in this codebase it is overloaded:
    - Linux driver framework concepts (`pci_driver`, file operations, module-as-driver),
    - guest-side virtio drivers,
    - existing RPC helper naming (`driver_rpc_*` in `rpc.h`).
  - Reusing `driver_*` for new core structs would increase ambiguity and code-reading friction.

- Why not `peer_*`?
  - Often implies symmetry/equality; roles in this path are operationally asymmetric.

- Why not `service_*`?
  - Technically workable, but tends to suggest service/web architecture vocabulary, less intuitive for kernel/device-model transport code.

### Comparison: QEMU / crosvm Style

Observed style alignment (high-level):

- They strongly type and name local objects they own (device model, queues, transport workers).
- The opposite side is usually described semantically (`guest`, `driver`, `frontend`) rather than represented as a concrete global runtime type unless needed.
- For split-process boundaries, `frontend/backend` naming is common.

Implication for this module:

- Keep concrete local types:
  - `device_endpoint`
  - `device_bundle`
  - `device_context`
- Keep remote side abstract in most code:
  - comments/docs: “guest-side consumer”, “driver-side”, “remote”
  - APIs: neutral verbs and fields (for example `notify_remote`, `remote_id`) where role specificity is unnecessary.

### Do We Need a First-Class Driver-Side Type?

Current recommendation: no, not by default.

- The current contract is transport/lifecycle/signaling based.
- A dedicated driver-side type should be introduced only if new functionality requires explicit remote-side state that cannot be naturally represented inside `device_context`.

## API Surface and Userspace Contract

UAPI ioctls and structs are defined in:
- `sources/kmod-sel4-virt/include/uapi/sel4/sel4_virt.h:45`
- `sources/kmod-sel4-virt/include/uapi/sel4/sel4_virt_types.h:11`

Important ioctls:
- VM and device control: `SEL4_CREATE_VM`, `SEL4_START_VM`, `SEL4_CREATE_VPCI_DEVICE`, `SEL4_SET_IRQLINE`
- Acceleration controls: `SEL4_IOEVENTFD`, `SEL4_IRQFD`
- Memory + IO orchestration: `SEL4_CREATE_IO_HANDLER`, `SEL4_WAIT_IO`, `SEL4_MMIO_REGION`

Core IOCTL dispatch path:
- `sources/kmod-sel4-virt/sel4_core.c:277`

## Lifecycle Model

### 1. Backend module init

- Initializes core infra (`sel4_init`) then PCI registers driver.
- Registers misc char device `/dev/sel4`, allocates workqueues, initializes irqfd support.

References:
- `sources/kmod-sel4-virt/pci/sel4_pci.c:476`
- `sources/kmod-sel4-virt/sel4_core.c:473`

### 2. Dataport probe and VMM assembly

- `sel4_pci_probe` maps BAR0/BAR1 as IOVA regions.
- Reads dataport identity from BAR0 event area name field.
- Infers `(vmid, dataport_type)` from name (`guest-iobuf-<id>` / `guest-ram-<id>`).
- Once both dataports exist, creates `sel4_vmm`, initializes RPC channels and memory maps, and inserts into pool.

References:
- `sources/kmod-sel4-virt/pci/sel4_pci.c:245`
- `sources/kmod-sel4-virt/pci/sel4_pci.c:309`
- `sources/kmod-sel4-virt/pci/sel4_pci.c:341`

### 3. Userspace VM acquisition

- Userspace opens `/dev/sel4` and issues `SEL4_CREATE_VM` with `sel4_vm_params`.
- Core asks backend server `create_vm`; PCI backend pulls a matching VMM from pool by id/ram size.
- Returns VM-specific fd backed by anonymous inode.

References:
- `sources/kmod-sel4-virt/sel4_core.c:376`
- `sources/kmod-sel4-virt/pci/sel4_pci.c:416`

### 4. Runtime processing

- Upcalls arrive from IRQ handler, enqueue global work item, then iterate all VMs and process pending RPC requests.
- Handled MMIO writes may stay in kernel (ioeventfd), others are forwarded to userspace queue.

References:
- `sources/kmod-sel4-virt/sel4_core.c:78`
- `sources/kmod-sel4-virt/sel4_core.c:46`
- `sources/kmod-sel4-virt/sel4_ioeventfd.c:206`

### 5. Teardown/removal

- VM refcount drops to zero on fd release; VM is removed from global list, IRQ disabled, workqueue drained, backend destroy callback invoked.
- PCI remove tries to detach associated VMM and notify core when dataport removal races VM lifetime.

References:
- `sources/kmod-sel4-virt/sel4_core.c:167`
- `sources/kmod-sel4-virt/sel4_core.c:360`
- `sources/kmod-sel4-virt/pci/sel4_pci.c:373`

## Data Path Analysis

### RPC request ingress and dispatch

1. Device-side RPC requests are consumed via `for_each_driver_rpc_req`.
2. Dispatcher checks `QEMU_OP(req->mr0)`.
3. For `QEMU_OP_MMIO`, tries `rpc_process_mmio`.
4. If not handled, forwards request to userspace via `user_rpc` forward queue.

References:
- `sources/kmod-sel4-virt/sel4_core.c:46`
- `sources/kmod-sel4-virt/include/uapi/sel4/rpc.h:79`
- `sources/kmod-sel4-virt/include/uapi/sel4/rpc.h:191`

### MMIO fast path (ioeventfd)

- Read operations are always forwarded to userspace.
- Write operations are matched against registered ioeventfd entries by `{addr_space, addr, len, data/wildcard}`.
- On match:
  - signal eventfd
  - send MMIO completion reply (`driver_rpc_ack_mmio_finish`)
- On miss or ack failure: returns unhandled and falls back to userspace path.

References:
- `sources/kmod-sel4-virt/sel4_ioeventfd.c:206`
- `sources/kmod-sel4-virt/sel4_ioeventfd.c:104`
- `sources/kmod-sel4-virt/include/uapi/sel4/rpc.h:289`

### IRQ injection path (irqfd)

- `irqfd` registers waitqueue callback on eventfd poll.
- On `POLLIN`, callback drains counter and issues `SEL4_IRQ_OP_PULSE` through RPC (`sel4_vm_set_irqline`).
- On `POLLHUP`, deferred cleanup work removes entry safely under VM lock.

References:
- `sources/kmod-sel4-virt/sel4_irqfd.c:40`
- `sources/kmod-sel4-virt/sel4_irqfd.c:27`
- `sources/kmod-sel4-virt/sel4_virt_drv.h:143`

### Memory mapping path

- `SEL4_CREATE_IO_HANDLER` returns anon fd for selected map index.
- `mmap` validates bounds and map type.
- IOVA mappings use `remap_pfn_range`; logical/virtual mappings fault-in page-by-page.

References:
- `sources/kmod-sel4-virt/sel4_core.c:232`
- `sources/kmod-sel4-virt/sel4_mmap.c:103`
- `sources/kmod-sel4-virt/sel4_mmap.c:80`
- `sources/kmod-sel4-virt/sel4_mmap.c:17`

## Concurrency and Synchronization

- Global VM list:
  - Protected with RW lock (`vm_list_lock`) during iteration/add/remove.
  - References: `sources/kmod-sel4-virt/sel4_core.c:33`, `sources/kmod-sel4-virt/sel4_core.c:124`
- Per-VM mutable structures (`ioeventfds`, `irqfds`, VMM state):
  - Protected with `vm->lock` spinlock wrappers.
  - References: `sources/kmod-sel4-virt/sel4_virt_drv.h:91`
- Deferred work:
  - I/O processing: one global work item on one unbound workqueue.
  - IRQFD cleanup: separate cleanup workqueue.

References:
- `sources/kmod-sel4-virt/sel4_core.c:35`
- `sources/kmod-sel4-virt/sel4_core.c:484`
- `sources/kmod-sel4-virt/sel4_irqfd.c:196`

## Findings and Risks

### High priority

1. Forwarding errors are effectively ignored in core dispatch.
- `rpc_process` notes FIXME and caller does not act on failure path.
- Potential impact: request loss/backpressure ambiguity when userspace-forward queue is full.
- Reference: `sources/kmod-sel4-virt/sel4_core.c:59`

2. ioeventfd deassign matches only `eventfd`, not the full registration key.
- Deassigning can remove an unintended mapping if same eventfd is reused for multiple addresses.
- Reference: `sources/kmod-sel4-virt/sel4_ioeventfd.c:176`

### Medium priority

3. Global single work item scans all VMs on notification.
- Can cause unfairness and latency coupling under multi-VM load.
- References: `sources/kmod-sel4-virt/sel4_core.c:35`, `sources/kmod-sel4-virt/sel4_core.c:78`

4. Dataport type parsing is permissive prefix matching.
- `strncmp` with minimum length can accept partial/malformed names.
- Reference: `sources/kmod-sel4-virt/pci/sel4_pci.c:75`

5. ID width mismatch (`__u64` API -> `int` internal pool selection).
- Large userspace IDs may truncate silently.
- References: `sources/kmod-sel4-virt/include/uapi/sel4/sel4_virt_types.h:11`, `sources/kmod-sel4-virt/pci/sel4_pci.c:416`

### Lower priority / design caveats

6. `sel4_vm_destroy_vpci` currently returns `-ENOSYS`.
- API exists but is not implemented in backend.
- Reference: `sources/kmod-sel4-virt/sel4_virt_drv.h:129`

7. MMIO ack failure in fast path falls back with TODO retry comment.
- Recoverability and ordering guarantees are not explicit.
- Reference: `sources/kmod-sel4-virt/sel4_ioeventfd.c:241`

## Strengths

- Clear separation of backend provider contract via `sel4_vm_server`.
- Efficient fast paths for common virtio operations (ioeventfd/irqfd).
- Well-structured shared-memory RPC queues with explicit ownership/reclaim semantics.
- VM resource lifetime tied to fd lifecycle with refcounting.

References:
- `sources/kmod-sel4-virt/sel4_virt_drv.h:28`
- `sources/kmod-sel4-virt/include/uapi/sel4/rpc_queue.h:442`
- `sources/kmod-sel4-virt/sel4_core.c:181`

## Recommended Hardening Order

1. Make forward-queue failures explicit and observable.
- Add robust error handling and tracepoints/counters in `rpc_process` path.

2. Fix ioeventfd deassign semantics.
- Match full tuple: `fd + addr_space + addr + len + data/wildcard`.

3. Tighten dataport name parsing.
- Validate exact grammar and full string consumption for both type and vmid.

4. Remove ID truncation risk.
- Use a wider type consistently for VM IDs in pool and matching paths.

5. Improve workqueue granularity.
- Consider per-VM work item or per-VM pending bitmask to avoid global scanning on every notify.

## Single-Device Migration (Both Sides)

This section specifies what must change if the transport is redesigned from:

- current: two virtual PCI devices per channel (`guest-iobuf-<id>`, `guest-ram-<id>`)
- target: one virtual PCI device per channel, migrated in phases:
  - Phase 1: below-4GiB, 32-bit BAR layout
  - Phase 2: optional 64-bit BAR extension when address space pressure requires it

### Current Two-Sided Contract (What Exists Today)

Device-side producer path (seL4/CAmkES):

- Two `camkes_crossvm_connection` entries are generated per channel:
  - iobuf/control: `guest-iobuf-<id>`
  - memdev/data: `guest-ram-<id>`
  - References:
    - `projects/virtioso-camkes-vm/templates/seL4VirtIODeviceVM.template.c:33`
    - `projects/virtioso-camkes-vm/templates/seL4VirtIODeviceVM.template.c:35`
    - `projects/virtioso-camkes-vm/templates/seL4VirtIODeviceVM.template.c:38`
- Dataport topology is explicitly split into `iobuf` and `memdev`:
  - `projects/virtioso-camkes-vm/configurations/virtioso/vm.h:50`
  - `projects/virtioso-camkes-vm/configurations/virtioso/vm.h:62`
- Cross-VM producer creates one vPCI function per connection, with BAR0=event, BAR1=dataport:
  - `projects/sel4_projects_libs/libsel4vmmplatsupport/src/drivers/cross_vm_connection.c:47`
  - `projects/sel4_projects_libs/libsel4vmmplatsupport/src/drivers/cross_vm_connection.c:61`
  - `projects/sel4_projects_libs/libsel4vmmplatsupport/src/drivers/cross_vm_connection.c:62`

Backend-side consumer path (Linux kmod):

- Expects two endpoints and pairs by event-bar name prefix + vmid:
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:75`
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:96`
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:339`
- Assumes two BAR resources per endpoint (`for (i = 0; i < 2; i++)`):
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:271`
- Builds runtime context only when both endpoint types are present:
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:180`
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:219`

QEMU device-side userspace:

- Uses existing UAPI memory-map abstraction (`RAM`, `IOBUF`, `EVENT_BAR`) and does not parse PCI BARs directly:
  - `sources/qemu/accel/sel4/sel4-all.c:445`
  - `sources/qemu/accel/sel4/sel4-all.c:505`
  - `sources/qemu/accel/sel4/sel4-all.c:531`

### Target Contract

Naming for planes uses generic terms:

- `control` plane (not `iobuf`)
- `data` plane (not `ram`)

This is intentionally transport-generic and not tied to virtio-only semantics.

Decisions locked for current implementation cycle:

- Keep existing event-bar behavior and register semantics unchanged in phase 1.
- Use single-device phase-1 BAR mapping:
  - `BAR0` event
  - `BAR1` data
  - `BAR2` control
- Keep BAR addresses below 4GiB in phase 1.
- Preserve backward compatibility only for vm-examples users of crossvm APIs.
- No compatibility requirement for existing virtioso split-device topology.
- Use `control`/`data` terminology in design and docs (transport-generic, not virtio-only).
- Do not add a new control metadata ABI in phase 1; use existing naming/ID plumbing.
- Keep equal BAR sizing for `BAR0/1/2` in phase 1.
- Move to 64-bit BAR support only when 32-bit address-space limits become a practical blocker.

Phase 1 contract (single device, 32-bit BARs under 4GiB):

1. `BAR0`: event BAR (keep existing event register behavior)
2. `BAR1`: data BAR (shared data window)
3. `BAR2`: control BAR (RPC queue region)

Phase 2 contract (optional):

- move to 64-bit BAR pairs when needed for address-space scale.

Keep kernel UAPI and QEMU `/dev/sel4` ioctl ABI unchanged in phase 1 (still expose
`SEL4_MEM_MAP_RAM`, `SEL4_MEM_MAP_IOBUF`, `SEL4_MEM_MAP_EVENT_BAR`; naming cleanup can be internal).

### Producer-Side Changes (seL4/CAmkES/libsel4vmmplatsupport)

1. Collapse channel definition from two connection records to one.

- Change `seL4VirtIODeviceVM.template.c` to emit one `camkes_crossvm_connection` per channel, not two:
  - today: `projects/virtioso-camkes-vm/templates/seL4VirtIODeviceVM.template.c:33`
- Extend `camkes_crossvm_connection`/`crossvm_handle` to carry two dataport handles:
  - control dataport handle
  - data dataport handle
- Affected headers/wrappers:
  - `projects/vm/components/VM_Arm/include/vmlinux.h`
  - `projects/vm/components/VM_Arm/src/crossvm.c`
  - `projects/vm/components/Init/include/crossvm.h`
  - `projects/vm/components/Init/src/crossvm.c`

2. Redesign vPCI BAR construction for one function with event/data/control windows.

- Replace BAR layout in `construct_connection_bar`:
  - today writes `.bar0` and `.bar1`: `projects/sel4_projects_libs/libsel4vmmplatsupport/src/drivers/cross_vm_connection.c:61`
  - phase 1 target writes `.bar0`, `.bar1`, `.bar2` (32-bit)
  - phase 2 target can switch to low/high paired BAR entries.
- Update `vmm_pci_bar_t bars[]` usage from `num_bars=2` to `num_bars=3` in phase 1.
- Remove dependence on name-based type split in event memory string payload (`guest-iobuf-*` / `guest-ram-*`).

3. Update reservation logic from (event + dataport) to (data + control).

- Replace split reserve flow:
  - `reserve_event_bar`: `projects/sel4_projects_libs/libsel4vmmplatsupport/src/drivers/cross_vm_connection.c:125`
  - `reserve_dataport_memory`: `projects/sel4_projects_libs/libsel4vmmplatsupport/src/drivers/cross_vm_connection.c:147`
- New flow:
  - reserve/map data dataport region
  - reserve/map control dataport region
  - keep existing event-bar naming field for ID plumbing in phase 1

4. PCI BAR sizing behavior (phase 1 finding).

- Current producer code explicitly enforces same `size_bits` to avoid Linux BAR remap:
  - `projects/sel4_projects_libs/libsel4vmmplatsupport/src/drivers/cross_vm_connection.c:230`
- Interpretation: with current vPCI integration, keeping equal BAR size remains necessary unless
  BAR relocation/rebasing support is strengthened end-to-end.
- Therefore in phase 1, keep event/data/control BAR size policy conservative (equal-sized resources,
  even if some space is reserved/unused).

Phase 2 requirement (only when moving to 64-bit BARs):
- implement full 64-bit BAR encoding/rewrite semantics in `pci_helper.c`.

### Consumer-Side Changes (kmod-sel4-virt)

1. Remove dual-endpoint pairing model in `pci/sel4_pci.c`.

- Delete/replace:
  - type parsing (`extract_dataport_type`), vmid parsing from names
  - global `sel4_dataports` pairing state machine
  - activate-on-pair logic
- References:
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:75`
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:96`
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:339`

2. Replace BAR probing assumptions.

- Current probe only scans first two resources:
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:271`
- New probe must:
  - in phase 1: parse at least BAR0/BAR1/BAR2 (event/data/control)
  - in phase 2: support 64-bit BAR pair parsing
  - validate BAR alignment and size constraints

3. Build runtime context from one endpoint.

- Current create path consumes two endpoint objects:
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:180`
- New path should map:
  - `SEL4_MEM_MAP_IOBUF` -> control BAR + iobuf offset
  - `SEL4_MEM_MAP_EVENT_BAR` -> control BAR + event offset
  - `SEL4_MEM_MAP_RAM` -> data BAR

4. Simplify teardown and pool interaction.

- Current remove path coordinates paired endpoint states:
  - `sources/kmod-sel4-virt/pci/sel4_pci.c:373`
- New remove path can be one-device-one-context (no pair transitions).
- `sel4_vmm_pool` can be simplified or eliminated depending on desired attach semantics:
  - `sources/kmod-sel4-virt/pci/sel4_vmm_pool.c:37`

5. Keep `sel4_core` and userspace ABI stable initially.

- `SEL4_CREATE_VM`, `SEL4_CREATE_IO_HANDLER`, `SEL4_WAIT_IO`, `SEL4_IOEVENTFD`, `SEL4_IRQFD` paths can remain unchanged:
  - `sources/kmod-sel4-virt/sel4_core.c:277`
  - `sources/kmod-sel4-virt/include/uapi/sel4/sel4_virt.h:45`

### QEMU-Side Impact

Direct impact in `sources/qemu/accel/sel4/sel4-all.c` is low if kmod continues to expose
the same 3 map indices and ioctls.

Potential updates:
- none required for basic operation if kmod performs BAR parsing/metadata translation.
- keep compatibility of kernel bootargs contract (`uservm=...`) unless changing memory map policy:
  - `sources/qemu/accel/sel4/sel4-all.c:586`

### CAmkES/Topology Impact

Current configuration macros are explicitly split (`iobuf` + `memdev`):
- `projects/virtioso-camkes-vm/configurations/virtioso/vm.h:50`
- `projects/virtioso-camkes-vm/configurations/virtioso/vm.h:62`

Target should introduce explicit control/data roles for one channel object, for example:
- `control_handle` (RPC + event + metadata)
- `data_handle` (shared RAM)

This requires template and macro updates in:
- `projects/virtioso-camkes-vm/configurations/virtioso/vm.h`
- `projects/virtioso-camkes-vm/templates/seL4VirtIODeviceVM.template.c`
- `projects/virtioso-camkes-vm/templates/seL4VirtIODriverVM.template.c` (if handle names change)

### Non-Virtioso Consumers

`cross_vm_connection` is also used in `projects/vm-examples/*`.
Decision: backward compatibility is required only toward vm-examples.
Compatibility policy (explicit):
- retain vm-examples single-dataport compatibility path
- do not retain old virtioso split-device compatibility path

Recommended approach:
- extend `crossvm_handle_t` with optional new fields appended at end
- keep existing initializers valid
- infer mode from populated fields (no explicit compatibility flags)

Reference users:
- `projects/vm-examples/apps/Arm/vm_introspect/src/cross_vm_connection.c`
- `projects/vm-examples/apps/Arm/vm_cross_connector/src/cross_vm_connections.c`

### High-Risk Areas

1. Linux PCI enumeration/reprogramming behavior if BAR sizes differ (phase 1).
2. vm-examples legacy path vs split path mode detection regressions.
3. Removal races during transition from pair-based to single-device lifecycle.
4. 64-bit BAR emulation correctness in `pci_helper.c` (phase 2).

### Suggested Implementation Sequence

1. Implement single-device phase-1 BAR layout (`BAR0` event, `BAR1` data, `BAR2` control), all <4GiB.
2. Keep/validate equal BAR size policy in producer to avoid guest remap issues.
3. Implement single-device producer path in `cross_vm_connection.c`.
4. Switch kmod probe/create/remove to single-device model.
5. Update virtioso templates/macros to generate one channel record with control+data handles.
6. Keep vm-examples compatibility via appended optional fields in `crossvm_handle_t` and structural mode inference.
7. Keep QEMU and UAPI stable; run integration tests.
8. Add/remove stress tests and document hot-remove behavior as follow-up TODO.
9. Phase 2: add 64-bit BAR support when required by address-space scaling.

### Concrete Code-Level Delta (Phase 1)

This section turns the migration into file-level implementation tasks for the selected phase-1 contract:

- one vPCI device per channel
- `BAR0=event`, `BAR1=data`, `BAR2=control`
- 32-bit BAR addresses under 4GiB
- keep event register semantics unchanged
- keep vm-examples compatibility

#### Shared API Delta (Producer + CAmkES Wrappers)

Primary change: represent one logical channel with two dataports (`control`, `data`) in the crossvm APIs.

Files:
- `projects/sel4_projects_libs/libsel4vmmplatsupport/include/sel4vmmplatsupport/drivers/cross_vm_connection.h`
- `projects/vm/components/VM_Arm/include/vmlinux.h`
- `projects/vm/components/Init/include/crossvm.h`

Recommended struct/API shape:

```c
/* libsel4vmmplatsupport/include/.../cross_vm_connection.h */
typedef struct crossvm_handle {
    /* canonical data plane dataport */
    crossvm_dataport_handle_t *dataport;

    /* control plane dataport (required in split mode) */
    crossvm_dataport_handle_t *control_dataport;
    uint32_t connection_id;

    emit_fn emit_fn;
    seL4_Word consume_id;
    const char *connection_name;
} crossvm_handle_t;
```

Notes:
- `dataport` is the SSOT/DRY data-plane handle for both legacy and split paths.
- split mode requires `control_dataport` in addition to `dataport`.
- `connection_id` replaces name-based vmid extraction in kmod.
- keep field append-only layout for minimal breakage in aggregate initializers.

`camkes_crossvm_connection` wrappers should mirror this split:
- VM_Arm wrapper (`vmlinux.h` + `VM_Arm/src/crossvm.c`)
- Init wrapper (`crossvm.h` + `Init/src/crossvm.c`)

Compatibility rule for vm-examples:
- If only legacy `handle`/`dataport` is provided:
  - leave `control_dataport = NULL`
  - keep `dataport` populated
  - legacy mode is inferred structurally

Mode selection rule (no explicit flags):
- split mode: `control_dataport != NULL` (and `dataport != NULL`)
- legacy mode: `control_dataport == NULL` and `dataport != NULL`
- invalid: `dataport == NULL`

#### Producer Runtime Delta (`cross_vm_connection.c`)

File:
- `projects/sel4_projects_libs/libsel4vmmplatsupport/src/drivers/cross_vm_connection.c`

`struct connection_info` changes:
- replace `dataport_address/dataport_size_bits` with:
  - `control_address`, `control_size_bits`
  - `data_address`, `data_size_bits`
- keep event fields unchanged.

Function-level edits:

1. `get_pci_bar_size(...)`
- compute max of:
  - event bar minimum (4KiB)
  - control dataport bytes
  - data dataport bytes
- phase-1 rule: keep equal `size_bits` for BAR0/1/2.

2. Reservation flow:
- split `reserve_dataport_memory(...)` into:
  - `reserve_control_memory(...)`
  - `reserve_data_memory(...)`
- `initialise_connections(...)` sequence per connection:
  1. reserve event page at `base + 0 * pci_bar_size`
  2. reserve data dataport at `base + 1 * pci_bar_size`
  3. reserve control dataport at `base + 2 * pci_bar_size`
  4. register consume callback as today

3. PCI config creation:
- `construct_connection_bar(...)`:
  - set `.bar0`, `.bar1`, `.bar2`
  - `vmm_pci_bar_t bars[3]`
  - `vmm_pci_create_bar_emulation(entry, 3, bars)`

4. Phase-1 ID plumbing:
- keep ID in event-bar naming field (no new metadata ABI in phase 1).
- producer writes `connection_name` as `guest-control-data-<id>`.
- kmod parses `<id>` from this string and no longer uses type pairing logic.

#### CAmkES Template/Configuration Delta

Files:
- `projects/virtioso-camkes-vm/templates/seL4VirtIODeviceVM.template.c`
- `projects/virtioso-camkes-vm/configurations/virtioso/vm.h`
- `projects/virtioso-camkes-vm/templates/seL4VirtIODriverVM.template.c` (naming alignment only)

Required changes:

1. Generate one connection entry per channel in `seL4VirtIODeviceVM.template.c`:
- from:
  - one `guest-iobuf-*`
  - one `guest-ram-*`
- to:
  - one channel record with:
    - `dataport = ..._memdev_handle` (canonical data plane)
    - `control_dataport = ..._iobuf_handle`
    - `connection_id = drv.id`
    - `connection_name = "guest-control-data-<id>"` (debug label)

2. Notification registration loop:
- currently iterates only the first half of the old array.
- after collapse, iterate all channel records directly (1:1 with channels).

3. Macro/naming cleanup in `vm.h`:
- keep dataport objects as-is for now (to limit churn),
- but rename config attributes/comments to control/data terminology where possible.
- keep phase-1 equal-size policy explicit:
  - `vm##_dev##_vm##_drv##_iobuf.size = VM..._VIRTIO_DATA_SIZE;`
  - document this as temporary BAR-sizing constraint.

#### VM Wrapper Delta (vm-examples Compatibility Boundary)

Files:
- `projects/vm/components/VM_Arm/src/crossvm.c`
- `projects/vm/components/Init/src/crossvm.c`

Required changes:
- when building `crossvm_handle_t`, populate new fields.
- populate canonical `dataport` from existing handle.
- set `control_dataport` only for split-device callers.
- enforce split invariant only in split path (`control_dataport` must be non-NULL).
- plumb `connection_id` from camkes struct when provided.

This keeps vm-examples callers working without requiring immediate source updates.

#### kmod Consumer Delta (`sources/kmod-sel4-virt`)

Primary file:
- `sources/kmod-sel4-virt/pci/sel4_pci.c`

Key structural change:
- replace `struct sel4_dataport` with one endpoint/context record per PCI function:
  - `vmid` (from event-bar string suffix)
  - `mem[3]` for event/data/control BARs
  - lifecycle state
  - `vmm_id` for core binding

Function-level edits:

1. Remove type/name parsing path:
- delete:
  - `extract_dataport_type()`
  - `extract_vmid()`
  - `dataport_match[]`
  - pair search logic in probe

2. BAR probing:
- map BAR0/1/2 explicitly.
- validate BAR presence/sizes.

3. Create vmm directly from one endpoint:
- old: `sel4_pci_vmm_create(int id, struct sel4_dataport *dataports[])`
- new: `sel4_pci_vmm_create(int id, struct sel4_endpoint *ep)`
- mappings:
  - `SEL4_MEM_MAP_EVENT_BAR <- ep->mem[BAR0]`
  - `SEL4_MEM_MAP_IOBUF <- ep->mem[BAR2]` (control)
  - `SEL4_MEM_MAP_RAM <- ep->mem[BAR1]` (data)

4. IRQ/doorbell wiring:
- unchanged behavior:
  - consume register from event BAR offset `0x4`
  - emit register at event BAR offset `0x0`

5. Teardown:
- simplify remove path to one endpoint lifecycle (no pair-state transitions).
- hot-remove race handling details are deferred; track as TODO before production hardening.

Secondary file:
- `sources/kmod-sel4-virt/pci/sel4_vmm_pool.c`

Minimal phase-1 change:
- keep pool API, keyed by `vmm->id` (`connection_id`), same as now.
- update comments from “pairing control+ram” to “single endpoint with control/data BARs”.

#### QEMU/kmod UAPI Delta

Files:
- `sources/qemu/accel/sel4/sel4-all.c`
- `sources/kmod-sel4-virt/include/uapi/sel4/sel4_virt.h`

Phase-1 target:
- no ABI change required.
- keep existing map indices and ioctls.
- only internal semantics change: `SEL4_MEM_MAP_IOBUF` maps from BAR2, `SEL4_MEM_MAP_RAM` maps from BAR1.

### Test Matrix (Must Be Covered in Docs/Execution)

1. vm-examples compatibility path:
- single dataport connection still probes and works.
- no `control_dataport` provided by vm-examples callers.

2. New single-device split path:
- one PCI function per channel with `BAR0/1/2 = event/data/control`.
- both dataports present and mapped correctly in kmod.

3. RPC/IRQ fast paths:
- ioeventfd write match path and fallback path.
- irqfd pulse path end-to-end.

4. Lifecycle:
- create/start/stop/release normal path.
- remove/reprobe path.
- hot-remove race behavior (document current behavior; hardening is TODO).

### Documentation Delta (What To Update Alongside Code)

When phase-1 code lands, update these docs in the same change window to avoid stale split-device guidance:

1. `docs/architecture/memory-model.md`
- replace “iobuf/memdev as separate connector devices” wording with “single connector device with control/data BARs”.

2. `docs/architecture/vm-topology.md`
- update channel diagrams to show one PCI function per channel (`event/data/control`), not two functions.

3. `docs/components/camkes-templates.md`
- update template snippets for `seL4VirtIODeviceVM.template.c` connection array shape.

4. `docs/reference/configuration.md`
- clarify phase-1 equal-size BAR policy and why control dataport size tracks data dataport size currently.

5. `projects/sel4_projects_libs/libsel4vmmplatsupport/docs/libsel4vmmplatsupport_cross_vm_connection.md`
- refresh `crossvm_handle_t` field documentation and initialization examples.

6. `docs/integration/kmod-sel4-virt-backend-analysis.md`
- keep this file as the migration SSOT and append implementation results/test outcomes after each phase.

## Quick File Map

- Core: `sources/kmod-sel4-virt/sel4_core.c`
- Internal API: `sources/kmod-sel4-virt/sel4_virt_drv.h`
- Backend PCI: `sources/kmod-sel4-virt/pci/sel4_pci.c`
- VMM pool: `sources/kmod-sel4-virt/pci/sel4_vmm_pool.c`
- ioeventfd: `sources/kmod-sel4-virt/sel4_ioeventfd.c`
- irqfd: `sources/kmod-sel4-virt/sel4_irqfd.c`
- mmap: `sources/kmod-sel4-virt/sel4_mmap.c`
- VMM alloc/validation: `sources/kmod-sel4-virt/sel4_vmm.c`
- RPC protocol: `sources/kmod-sel4-virt/include/uapi/sel4/rpc.h`
- RPC queue internals: `sources/kmod-sel4-virt/include/uapi/sel4/rpc_queue.h`

## Implementation Status (Current Workspace)

Completed in code:

1. Producer-side split support in `libsel4vmmplatsupport`:
- `crossvm_handle_t` extended with optional `control_dataport`.
- BAR emulation now supports:
  - legacy mode: `BAR0 event`, `BAR1 data`
  - split mode: `BAR0 event`, `BAR1 data`, `BAR2 control`
- Mode is inferred structurally (`control_dataport != NULL`), no explicit mode flags.
- Commit: `projects/sel4_projects_libs` `6ccd231`

2. VM wrapper plumbing (`projects/vm`):
- Added optional `control_handle` to both VM_Arm and Init crossvm connection structs.
- Existing vm-examples-compatible callers remain valid (field appended, optional).
- Wrapper code maps `handle -> dataport` and `control_handle -> control_dataport`.
- Commit: `projects/vm` `3188afb`

3. Virtioso template switch to single connector entry per channel:
- `seL4VirtIODeviceVM.template.c` now emits one connection per channel using:
  - `handle = memdev` (data plane)
  - `control_handle = iobuf` (control plane)
- Event BAR name now uses `guest-device-<id>`.
- Commit: `projects/virtioso-camkes-vm` `7c6abd1`

4. kmod consumer migration (`sources/kmod-sel4-virt`):
- Removed legacy two-device pairing by `guest-iobuf`/`guest-ram`.
- Switched to single PCI function with BAR parsing:
  - `BAR0 event`, `BAR1 data`, `BAR2 control`
- Memory map wiring:
  - `SEL4_MEM_MAP_RAM <- BAR1`
  - `SEL4_MEM_MAP_IOBUF <- BAR2`
  - `SEL4_MEM_MAP_EVENT_BAR <- BAR0`
- mappable IO-handler names aligned to:
  - `guest-data`, `guest-control`, `guest-event-bar`
- Commit: `sources/kmod-sel4-virt` `63ac868`

5. Yocto module build path automation:
- Added workspace target: `make kmod-sel4-virt`
- Added script: `virtioso-build/scripts/build_yocto_kmod_sel4_virt.sh`
  - default clean rebuild (`cleansstate`)
  - optional incremental with `YOCTO_INCREMENTAL=1`
- Docs/runbook/router updated accordingly.
- Commits:
  - `virtioso-build` `befdb4f`
  - `projects/virtioso-camkes-vm` `281e4ec`

Validation completed:

- `make kmod-sel4-virt` from workspace root succeeded and produced:
  - module: `vm-images/build/tmp/work/vm_jetson_agx_orin-poky-linux/kernel-module-sel4-virt/1.0/image/lib/modules/5.15.148-l4t-r36.4-1012.12+g8dc079d5c8c4/updates/sel4_virt.ko`
  - RPMs under: `vm-images/build/tmp/deploy/rpm/vm_jetson_agx_orin/`

Remaining validation:

1. Full `vm_qemu_virtio` clean rebuild and runtime validation on Orin AGX profile to confirm producer-side BAR layout + guest-side consumption end-to-end.
2. Hot-remove race hardening remains TODO (as intentionally deferred in phase 1).
