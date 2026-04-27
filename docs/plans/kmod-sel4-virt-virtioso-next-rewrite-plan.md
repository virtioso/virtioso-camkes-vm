# kmod-sel4-virt Rewrite Plan on `virtioso-next`

## Purpose

This note groups the current `sources/kmod-sel4-virt` work after
`virtioso-next` into rewrite topics that can be replayed deliberately on top of
the current `virtioso-next` branch. The work is not limited to the Linux kmod:
the same topic stack crosses QEMU, shared contracts, the CAmkES/VMM side, trace
support modules, and Yocto integration.

The observed local range was:

```text
virtioso-next..everything
base: fc39ef8 irqfd: use fd_file accessor
head: 66afe76 trace: log kmod mailbox mapping
```

This is a planning document, not command authority. Build and test commands
remain governed by `AGENTS.md` and `docs/agents/*`.

## Cross-Repo Branch Evidence

The kmod range is the visible tip of a multi-repository program. Local reflogs
and branch names show these relevant carriers:

| Repository | Relevant branch/history | Role in rewrite |
| --- | --- | --- |
| `sources/kmod-sel4-virt` | `virtioso-next-tracing`, rebased locally as `everything` onto `virtioso-next` | Linux `/dev/sel4` API, PCI/DT transport, backend mailbox consumer, direct delegation, trace bridge |
| `sources/qemu` | `virtioso-next-tracing` | seL4 accelerator userspace side, direct delegation consumer, trace shard init, backend request ioctls, virtio/physmem tracing |
| `sources/virtioso-contracts` | `virtioso-next` and `virtioso-next-tracing` contain the shared contract stack | Shared RPC, direct MMIO slots, backend/control mailbox layout, trace event and phase IDs |
| `sources/kmod-vio-trace` | `virtioso-next` and `virtioso-next-tracing` | Canonical `vio_trace` Linux module, DT shard identity, `vio_trace_open_shard_fd()` provider |
| `projects/virtioso-camkes-vm` | older `virtioso-next-tracing` history plus current docs on `virtioso-next` | CAmkES composition, VMM-side mailbox producer/consumer, trace arming/control, DT/reserved-memory generation, runbooks |
| `vm-images/virtioso-yocto-layers` | `virtioso-next-tracing`, `walnascar`, and `virtioso-next` contain integration pieces | Packages kmod/QEMU/contracts/vio-trace into VM images and exposes local-source recipes |
| `sources/sel4-linux-kernel-support` | `virtioso-next` equals `virtioso-next-tracing` | Dedicated trace syscall wrappers used by guest-side tracing support |
| `projects/sel4_projects_libs` | `virtioso-next-tracing` | VMM library support: cross-VM connector BARs, cacheable mappings, GIC/x86 IRQ plumbing, fault tracepoints |
| `kernel`, `projects/seL4_libs`, `tools/seL4` | tracing or support branches exist, but include broader platform/debug work | Support infrastructure; include only narrowly required tracing/cache/syscall slices in this rewrite |

The rewrite should therefore be planned as a manifest-level stack. A kmod-only
replay can compile but will not validate the behavior unless the matching
contract, QEMU, VMM, trace-provider, and image-integration slices are aligned.

## Contracts-First SSOT Rule

All rewrite commits must be created on top of the current intended
`virtioso-next` branch in each participating repository. Do not replay work
from side branches directly as the final history. If a repo is currently on a
different branch, such as local `sources/kmod-sel4-virt` on `everything`, first
switch or recreate the work at the tip of that repo's `virtioso-next`, then add
the rewrite commit there.

The rewrite should first move headers and protocol definitions that are already
common to `sources/kmod-sel4-virt`, `sources/qemu`, and the seL4 VMM side into
`sources/virtioso-contracts`. The goal is to stop maintaining separate copies
before piling up new feature work.

This first milestone is not "land every contract change". It is an SSOT
extraction:

1. Inventory the duplicated or mirrored header surfaces in kmod, QEMU, and VMM
   code.
2. Move the common baseline definitions into `sources/virtioso-contracts`.
3. Switch all current consumers to include `virtioso-contracts`.
4. Verify the behavior is unchanged before adding direct delegation, backend
   mailbox, DT transport, or tracing extensions.

Implementation note, 2026-04-27:

- `sources/kmod-sel4-virt` must be based on its `virtioso-next` tip before the
  wrapper change; the local `everything` branch is only source history.
- Local `sources/virtioso-contracts` has been rewritten so `virtioso-next`
  starts with `6985775 rpc: bootstrap shared contract headers`: only the shared
  RPC/RPC queue baseline. The previous local branch tip is preserved as
  `backup-virtioso-next-before-contracts-first-rewrite`.
- Use `backup-virtioso-next-before-contracts-first-rewrite` as the local source
  branch for later contract replay. Inspect it with:
  `git -C sources/virtioso-contracts log --oneline virtioso-next..backup-virtioso-next-before-contracts-first-rewrite`.
- Later mailbox and direct-MMIO-slot definitions must remain separate
  consuming-topic slices, replayed only when their kmod/QEMU/VMM consumers are
  rewritten to use them.
- Existing `<sel4/rpc.h>` and `<sel4/rpc_queue.h>` include paths may remain as
  compatibility wrappers, but their definitions should come from
  `sources/virtioso-contracts`.

After that baseline extraction, `sources/virtioso-contracts` still must not be
treated as one monolithic prerequisite. Its later commits contain incremental
ABI and helper changes for several topics, so those later contract commits must
travel with the feature that first consumes them.

Observed contract groups:

- Bootstrap and include-safety:
  - `9d34394 trace(contracts): bootstrap shared virtioso rpc/trace headers`
  - `9c17e5a trace(contracts): make shared trace headers kernel-safe with pragma once`
- Generic MMIO trace helpers and early event IDs:
  - `00460d2 trace(contract): align virtio-console names and add mmio helpers`
  - `317117a trace(contract): add ADVANCE/RESTART_VCPU_FAULT event ids`
  - `08a13ea trace: add rpc forward event id`
  - `1fc8640 rpc: add dequeue state trace event`
  - `4522ab0 virtioso-contracts: add RPC forward queue trace ids`
- RPC queue ordering and cache/coherency helpers:
  - `7e78102 rpc: publish queue payload before head advance`
  - `81f539b rpc: sync shared iobuf before doorbell`
  - `5fe2d2f rpc: sync shared payload before queue publish`
  - `474b472 rpc: sync queue marker after publish commit`
  - `8ecb8fa rpc: sync shared state during dequeue`
  - `cf89912 rpc: roll back shared RPC helpers toward bb272fb`
- Direct MMIO slot transport:
  - `7c50bed rpc: add direct mmio slot transport`
  - `a204426 rpc: use kernel-safe cas for mmio slot claim`
  - `eb04ddf trace: add mmio slot lifecycle event`
- Backend/control mailbox:
  - `9eb8025 contracts: define backend mailbox slots`
  - `b3f54f1 contracts: keep backend mailbox word layout compatible`
  - `baf6bb0 contracts: add mailbox slot helpers`
  - `77e19e7 rpc: add fixed-slot control mailbox`
- Backend/QEMU/VMM trace phase IDs:
  - `339da78 trace: add backend mmio lifecycle contract`
  - `3765de6 trace: add virtio ioeventfd backend phases`
  - `31b0fae trace: add guest and host vring visibility phases`
  - `149af6d trace: add shared-memory mapping phases`
  - `cd9cd26 virtioso-contracts: add guest virtio-pci probe phases`
  - `b8a9ef1 trace: add host avail ring read phases`
  - `dd18e29 trace: add physmem slow-path phase ids`
  - `921a0c7 trace: add cached iommu translation phases`
  - `318ec8a trace: add cached init phase ids`
  - `998115b trace: add guest dma allocation phase ids`
  - `3b3e125 trace: add modern virtio probe phases`
  - `04bd980 tracing: add backend notify trace phase ids`
  - `6517973 tracing: add backend slot state phase ids`
  - `83e257b tracing: add vmm slot reuse phase ids`
  - `3c2e511 trace: add backend raw slot phase ids`
  - `b640c50 trace: add mailbox sync phase ids`
  - `a811067 trace: add mailbox mapping phase ids`
  - `899fbb4 trace: add dataport flush frame phase ids`

Rewrite implication:

- First create a shared-contract SSOT baseline from the definitions already
  common to kmod, QEMU, and the VMM.
- Do not merge all of `sources/virtioso-contracts virtioso-next` as the baseline
  extraction.
- For every contract change, identify the first kmod/QEMU/VMM/Yocto consumer and
  land the contract in the same slice or immediately before that consumer.
- Treat contract commits as ABI changes: each one needs at least one compile
  consumer and, when it changes shared-memory layout or ordering, one runtime
  validation consumer.
- Do not replay the old RPC/cache-sync workaround family unless a separate
  non-shareability correctness bug is proven. The Orin AGX coherency failure
  that motivated those commits was improper shareability attributes, later fixed
  in the elfloader/seL4 mapping path, so explicit cache maintenance would now
  add complexity and could mask future mapping regressions.
- Do not replay fixed-slot transports that carry `generation` fields. The
  old direct-MMIO-slot and backend-mailbox designs both grew during the same
  cache/shareability debugging period and both added generation-based lifetime
  tracking that is not wanted in the current rewrite.

## Topic Inventory

### 1. Single PCI Device With Event/Data/Control BARs

Current post-`virtioso-next` work replaces the older two-device PCI discovery
model with one backend PCI device that exposes three regions:

- BAR0: event/doorbell registers
- BAR1: data/RAM window
- BAR2: control/RPC window

Rewrite status, 2026-04-27:

- `sources/virtioso-contracts`: `eeb8a36 backend: define single pci endpoint
  layout` defines the shared BAR order, event BAR register indexes, and
  `guest-device-` endpoint name prefix.
- `sources/kmod-sel4-virt`: `cb128e9 pci: use single virtioso endpoint
  device` consumes that shared contract and switches PCI discovery from matched
  `guest-iobuf-*` / `guest-ram-*` devices to one `guest-device-*` endpoint
  with event/data/control BARs.
- `projects/virtioso-camkes-vm`: the active
  `templates/seL4VirtIODeviceVM.template.c` already emits single connector
  entries named `guest-device-<id>` and passes data plus control handles in one
  `camkes_crossvm_connection` entry.
- `sources/qemu`: no current `accel/sel4` source change was found for this
  slice; QEMU continues to consume the kmod `/dev/sel4` API rather than
  defining the endpoint BAR layout directly.

Evidence in the post-range implementation:

- `sources/kmod-sel4-virt/pci/sel4_pci.c`
- commit: `b8afe92 kmod: switch backend to single-device event/data/control BAR model`
- matching CAmkES-side history:
  - `projects/virtioso-camkes-vm`: `7f3742e docs: lock single-device BAR order and compatibility scope`
  - `projects/virtioso-camkes-vm`: `7c6abd1 templates: switch virtio device VM to single connector entries`
- matching VMM library history:
  - `projects/sel4_projects_libs`: `6ccd231 libsel4vmm: add split control/data BAR support`

Architectural role:

- Makes the transport shape match the intended device bundle instead of
  discovering separate data and control devices and pairing them later.
- Reduces identity coupling through PCI device names.
- Provides the memory layout later reused by the DT backend.

Rewrite note:

- This should be the first functional topic after rebasing because DT transport
  and mailbox work assume the three-region layout.

### 2. DT Transport and Runtime Backend Selection

The range adds a Device Tree transport alongside the PCI backend and a module
backend selector:

- `backend=auto`
- `backend=dt`
- `backend=pci`

In `auto` mode, the current post-range implementation prefers DT when
`CONFIG_OF` is available and falls back to PCI when only `CONFIG_PCI` is
available.

Rewrite status, 2026-04-27:

- `sources/virtioso-contracts`: `f91ac6e backend: define dt rpc endpoint
  binding` defines the shared DT node prefix, compatible string, and driver VM
  ID property used by producers and consumers.
- `sources/kmod-sel4-virt`: `70ebe5a transport: add dt backend selector`
  adds the OF platform backend, keeps PCI as a selectable backend, and defaults
  `backend=auto` to DT when `CONFIG_OF` is enabled.
- `projects/virtioso-camkes-vm`: the active
  `templates/seL4VirtIODeviceVM.template.c` now emits one
  `virtioso,sel4-camkes-rpc` node per VM virtio channel, with event/data/control
  regions ordered to match the shared BAR contract.
- `sources/qemu`: no current source change has been identified for this slice.
  QEMU remains outside the DT discovery path unless a later topic changes how
  backend endpoints are surfaced to userspace.
  Older QEMU history has `7304e371fa Do not load DTB on seL4 guest`; that is an
  adjacent guest boot-DTB suppression for SWIOTLB behavior, not the DT endpoint
  discovery/backend-selection rewrite itself.

Evidence:

- `sources/kmod-sel4-virt/transport/sel4_dt.c`
- `sources/kmod-sel4-virt/transport/sel4_transport_select.c`
- `projects/virtioso-camkes-vm/templates/seL4VirtIODeviceVM.template.c`
- commits:
  - `67b876e sel4-virt: add DT-default transport backend selector`
  - `b564746 sel4-virt: fix DT backend kernel compatibility`
  - `1937890 kmod: skip OF/PCI transport guard during clean goals`
  - `ff5561a kmod: drop OF/PCI configure-time Makefile guard`
  - `b013ac5 camkes: emit sel4-camkes-rpc DT endpoint from device template`

Architectural role:

- Separates endpoint discovery from the core `/dev/sel4` VM API.
- Allows Orin-style static DT composition and legacy PCI discovery to coexist.
- Turns PCI from the only discovery path into one backend under a common module.

Rewrite note:

- Keep the PCI path buildable and testable while adding DT.
- Avoid making `CONFIG_OF` and `CONFIG_PCI` mutually exclusive in the build.
- Treat backend selection as a compatibility boundary: userspace should keep
  using the same `/dev/sel4` VM API unless a later topic intentionally extends
  it.
- Do not replay the old event-BAR cacheability change. That was part of a
  futile attempt to fix an Orin AGX issue whose actual cause was improper
  shareability attributes.

DT correctness follow-up status, 2026-04-27:

- `projects/virtioso-camkes-vm`: the FDT node generation loop now continues
  after generating one matching `_fdt_node` entry. This is required once the
  VMM template can register one DT RPC endpoint per VM virtio channel.
- The old kmod commit `3474045 sel4: make event bar uncached and DT-sized` is
  intentionally not replayed because its functional change is cacheability
  plumbing.

### 3. Shared RPC and Trace Contract Headers

The first rewrite milestone should remove local or mirrored copies of shared
RPC, queue, trace, and backend protocol headers from the kmod/QEMU/VMM boundary
and consume the common contracts from `sources/virtioso-contracts`.

Evidence:

- `sources/kmod-sel4-virt/Makefile`
- `sources/kmod-sel4-virt/sel4_virt_drv.h`
- commit: `37404a7 trace(contracts): switch kmod to shared virtioso rpc headers`
- matching bootstrap contract history:
  - `sources/virtioso-contracts`: `9d34394 trace(contracts): bootstrap shared virtioso rpc/trace headers`
  - `sources/virtioso-contracts`: `9c17e5a trace(contracts): make shared trace headers kernel-safe with pragma once`

Architectural role:

- Makes shared memory protocol definitions owned by one source repo instead of
  by whichever side last edited its copy.
- Reduces drift between Linux kmod, QEMU, and seL4-side VMM protocol views.
- Provides a stronger base for tracing and mailbox protocol work.

Rewrite note:

- This is the first real rewrite milestone after baseline SHA capture.
- The early scope is common-header extraction and consumer rewiring, not feature
  expansion.
- Later contract changes such as
  direct MMIO slots, backend mailbox layout, control mailbox helpers, and trace
  phase IDs belong with their feature topics.
- The shared-contract baseline should compile in kmod, QEMU, and VMM consumers
  before adding tracing-heavy changes, otherwise feature failures can hide
  contract import mistakes.

### 4. Backend Mailbox ABI and Control Mailbox

This topic adds a fixed shared-memory mailbox in the control window so the
seL4-side device VMM can publish backend MMIO/config-space requests to the
driver VM, and the kmod can claim, execute, and complete them.

The ABI is not just a new queue name. It changes the control window layout:

```c
rpcmsg_iobuf_t rpc_iobuf;
virtioso_backend_mailbox_t backend_mailbox;
virtioso_control_mailbox_t control_mailbox;
```

The first mailbox is request/response oriented. The VMM publishes backend work
into fixed slots:

```c
typedef struct virtioso_backend_request {
    virtioso_backend_word_t addr;
    virtioso_backend_word_t value;
    uint64_t generation;
    uint32_t addr_space;
    uint16_t width;
    uint8_t direction;
    uint8_t reserved0;
} virtioso_backend_request_t;

typedef struct virtioso_backend_slot {
    volatile uint32_t state;
    uint32_t reserved0;
    virtioso_backend_request_t request;
    virtioso_backend_completion_t completion;
} virtioso_backend_slot_t;
```

Slot state is a four-state handshake:

```c
IDLE -> PENDING -> CLAIMED -> COMPLETE -> IDLE
```

The second mailbox is one-way control/event publication. It replaces the older
device-event queue path for DT control operations:

```c
typedef struct virtioso_control_event {
    uint32_t op;
    uint32_t reserved0;
    virtioso_backend_word_t mr1;
    virtioso_backend_word_t mr2;
    virtioso_backend_word_t mr3;
} virtioso_control_event_t;
```

Evidence in old source history:

- `sources/virtioso-contracts`:
  - `9eb8025 contracts: define backend mailbox slots`
  - `b3f54f1 contracts: keep backend mailbox word layout compatible`
  - `baf6bb0 contracts: add mailbox slot helpers`
  - `77e19e7 rpc: add fixed-slot control mailbox`
- `sources/kmod-sel4-virt`:
  - `05265bc dt: validate backend mailbox control window`
  - `878d176 dt: fix backend mailbox probe format`
  - `b612bbd dt: process async requests from backend mailbox`
  - `a5f1eb8 backend: ring existing dt doorbell on completion`
  - `8605fe9 rpc: publish dt control ops via mailbox`
- `projects/virtioso-camkes-vm`:
  - `5a551fa rpc: publish async requests through backend mailbox`
  - `74a0d35 rpc: bridge backend completions in device vmm`
  - `ee1527f rpc: bridge blocking emulation through backend mailbox`
  - `e893020 trace: add vmm backend mmio bridge events`

Architectural role:

- Separates backend MMIO/config request transport from the legacy RPC queue
  semantics.
- Lets the kmod process backend requests found in the DT control window without
  requiring userspace API changes.
- Preserves request identity with a `generation` field so completions can be
  matched to async or blocking VMM-side bridge entries.
- Keeps DT completion notification on the existing DT doorbell path:
  `vm->vmm->rpc.doorbell(vm->vmm->rpc.doorbell_cookie)`.

Rewrite decision, 2026-04-27:

- Do not replay the old backend mailbox ABI as-is.
- The `generation` field is explicitly rejected for the current rewrite.
- The useful historical idea is only the async ownership shape: a producer can
  publish a request into an owned slot and a consumer can complete it later.
  That does not require a separate generation counter if slot ownership remains
  strict and the existing async token or slot index is enough to identify the
  response.
- Treat the old mailbox commits as source evidence, not as implementation to
  cherry-pick. If a later feature proves it needs async backend decoupling,
  design a smaller slot transport with no generation field and no cache/debug
  baggage.

### 5. Direct MMIO Slot Transport

The old direct MMIO slot topic moved guest MMIO requests out of queued
`QEMU_OP_MMIO` messages and into fixed records embedded in `rpcmsg_iobuf_t`:

```c
typedef struct vso_mmio_request {
    seL4_Word addr;
    seL4_Word value;
    uint64_t generation;
    uint32_t addr_space;
    uint16_t width;
    uint8_t direction;
    uint8_t reserved0;
} vso_mmio_request_t;

typedef struct vso_mmio_slot {
    volatile uint32_t state;
    uint32_t reserved0;
    vso_mmio_request_t request;
    vso_mmio_completion_t completion;
} vso_mmio_slot_t;
```

Its state machine is the same family as the backend mailbox:

```c
IDLE -> PENDING -> CLAIMED -> COMPLETE -> IDLE
```

Evidence in old source history:

- `sources/virtioso-contracts`:
  - `7c50bed rpc: add direct mmio slot transport`
  - `a204426 rpc: use kernel-safe cas for mmio slot claim`
  - `eb04ddf trace: add mmio slot lifecycle event`
- `sources/kmod-sel4-virt`:
  - `268697f sel4-virt: add opt-in direct mmio delegation path`
  - `6c5b1cc trace: emit kernel mmio slot lifecycle`
- `projects/virtioso-camkes-vm`:
  - `8d06d73 trace: record mmio slot cutover and fix ioack handoff race`
  - related planning history records the slot cutover as removing queue-backed
    MMIO request/reply traffic from the active path.

Objective assessment:

- The real architectural benefit would be bypassing the generic RPC queue for
  MMIO hot-path requests and allowing multiple independently owned in-flight
  MMIO records.
- The immediate cost is another shared-memory ABI, another slot lifecycle, and
  the same unwanted `generation` lifetime scheme as the backend mailbox.
- The surrounding history is tightly coupled to trace and cache/shareability
  investigation work, so it is risky to replay as a functional improvement
  without a fresh performance or correctness requirement.

Rewrite decision, 2026-04-27:

- Do not replay the old direct MMIO slot transport as-is.
- Do not carry `generation` into the current ABI.
- Do not import MMIO slot lifecycle tracing; it was diagnostic scaffolding for
  the old slot design.
- Reconsider this topic only if a current benchmark or required feature proves
  that queued MMIO is a real bottleneck or semantic blocker. In that case, start
  from a minimal no-generation async ownership design rather than the old commit
  series.

### 6. Trace Framework Extraction

The trace framework can still be useful, but it must be separated from the old
event taxonomy. The old trace phase IDs describe cache/shareability,
generation-based slots, backend mailbox scans, and stale visibility hypotheses.
Those hypotheses are no longer the target architecture.

Framework pieces worth preserving or re-extracting:

- shared trace record and buffer layout;
- trace shard identity: VM id, execution domain, driver VM id, flags;
- DT-described shard discovery for guest-visible trace memory;
- `kmod-vio-trace` as the Linux-side trace shard provider and debugfs fallback;
- anonymous-fd shard export for userspace tools when explicitly requested;
- binary transfer framing and timeline tooling if independent of old event IDs.

Framework evidence in current or old history:

- `sources/kmod-vio-trace` is already a focused framework repo on
  `virtioso-next`:
  - `c8d345f vio-trace(ws-k2): drive shard identity from DT properties`
  - `011e028 vio-trace: support multi-node shard matching by tuple`
  - `f5633cd vio-trace: initialize guest header for explicit shard tuples`
  - `6ce10f4 vio-trace: gate shard fd export on mmap readiness`
  - `48a16a8 vio-trace: cap guest shard header capacity at canonical 32768`
- `projects/virtioso-camkes-vm` has framework-level VMM/export pieces:
  - `9a1298b camkes: add generic vio trace sink/source macros`
  - `15a78cd trace: support anon source GPA mapping and DT export module`
  - `fe7b751 trace(camlkes): advertise canonical vio_trace DT compatible`
  - `cbd2e36 vio-trace(tooling): add canonical package and CLI scaffold`
  - `e6524fd trace(vio): cut over non-kernel producers to
    VIO_TRACE_STREAM binary transport`
- `sources/virtioso-contracts` currently has no trace headers on the rewritten
  `virtioso-next` tip. That is intentional: trace contracts can be reintroduced
  as a minimal framework ABI instead of replaying the old event/phase list.

Old trace content to reject by default:

- cache/shareability trace phases;
- mailbox mapping/sync phases;
- backend mailbox slot scan phases;
- direct MMIO slot lifecycle phases;
- queue visibility phases whose purpose was proving stale cache behavior;
- QEMU/VMM/kmod producers tied only to rejected mailbox or direct-slot paths.

Rewrite decision, 2026-04-27:

- Keep tracing as an observability framework, not as a replay of old
  cache-debug event IDs.
- First trace slice should define only stable framework contracts:
  record/header format, source identity, domain constants, and shard discovery
  properties.
- Event IDs are added later only when a retained runtime path needs a concrete
  tracepoint. No bulk import of `virtioso-contracts` phase IDs.
- `kmod-vio-trace` may be retained mostly as-is if its DT tuple matching,
  header initialization, debugfs exposure, and shard-fd export build cleanly
  against the minimal contract.

Rewrite status, 2026-04-27:

- `sources/virtioso-contracts`: `8d16eb9 trace: define minimal framework
  contract` adds `<virtioso/trace/trace.h>` with only stable framework
  definitions:
  - compatible strings for `vio_trace` DT nodes;
  - shard identity property names;
  - guest EL1/EL0 execution-domain constants;
  - guest trace buffer and entry layout;
  - the canonical guest shard capacity cap.
- `sources/kmod-vio-trace`: `f6f834c vio-trace: consume shared trace
  framework contract` removes private copies of those constants/layouts and
  includes the shared contract instead.
- No event IDs or trace phase IDs were introduced in this slice.
- Local module build still requires the existing `sel4/sel4-support.h`
  dependency from the seL4 kernel-support sysroot; the contract include itself
  was compile-checked separately.

### 7. Trace Shard Bridge and EL0 Trace Export

This is related to tracing, but it is a distinct UAPI and memory-export topic.
The range adds a `/dev/sel4` trace-shard open path and a bridge provider that
discovers trace shard memory from DT and exposes it through an mmap-capable
anonymous fd.

Evidence:

- `sources/kmod-sel4-virt/include/uapi/sel4/sel4_virt.h`
- `sources/kmod-sel4-virt/sel4_trace_bridge.c`
- commits include:
  - `5a431c6 sel4-virt(uapi): define trace shard bridge ioctl contract`
  - `b039e45 sel4-virt: add deterministic trace-open-shard ioctl stub`
  - `dea1a3a sel4-virt: add trace-shard provider hook and bridge tests`
  - `839a173 sel4-virt: add trace shard bridge backend fd provider`
  - `dd38840 sel4-virt(trace): own shard-open provider in /dev/sel4 runtime path`
  - `4d9f573 sel4-virt(trace): initialize EL0 shard header in bridge-owned provider`
- matching trace-provider history:
  - `sources/kmod-vio-trace`: `0d71335 vio-trace(ws-k2): add shard-open runtime API with deterministic selection`
  - `sources/kmod-vio-trace`: `c8d345f vio-trace(ws-k2): drive shard identity from DT properties`
  - `sources/kmod-vio-trace`: `011e028 vio-trace: support multi-node shard matching by tuple`
  - `sources/kmod-vio-trace`: `f5633cd vio-trace: initialize guest header for explicit shard tuples`
- matching QEMU history:
  - `sources/qemu`: `18a8dcdf7c sel4: introduce vio_trace_init abstraction in qemu accel`
  - `sources/qemu`: `5ba597b2a5 sel4: try vmfd trace bridge before legacy tracebuffer device`
  - `sources/qemu`: `943d391d0f sel4: make vio trace init bridge-only`

Architectural role:

- Makes trace memory an explicit exported resource instead of an implicit kmod
  side effect.
- Supports guest EL0 trace consumers while keeping shard selection mediated by
  `/dev/sel4`.
- Introduces DT-described trace shard identity: VM id, execution domain, and
  driver VM id.

Rewrite note:

- Rebase this only after the trace framework contract is minimal and stable.
- Keep the UAPI small and deterministic.
- Preserve clear rejection behavior for unsupported execution domains and
  mismatched VM ids.
- Do not make the bridge depend on rejected mailbox, direct-MMIO-slot, or
  generation-based protocols.

### 8. Direct MMIO and Backend Request Delegation

The range adds an opt-in direct delegation mode where unhandled MMIO/backend
requests can be exposed through the VM fd rather than only through the older
RPC forwarding queue.

Evidence:

- `sources/kmod-sel4-virt/sel4_core.c`
- `sources/kmod-sel4-virt/include/uapi/sel4/sel4_virt.h`
- commits include:
  - `177ab88 sel4-virt: add direct delegation vmfd scaffold`
  - `268697f sel4-virt: add opt-in direct mmio delegation path`
  - `0621025 sel4-virt: expose queue wakeups via vmfd poll`
  - `a14e528 sel4-virt: test vmfd poll for queued mmio`
  - `e531c0a sel4_virt: expose VM mode for direct delegation`
  - `0f1f1f5 kmod: process mmio via direct shared slots`
- matching QEMU history:
  - `sources/qemu`: `88fc023aa3 qemu: add direct delegation vmfd handler scaffold`
  - `sources/qemu`: `5d969a1550 qemu: use vmfd handlers instead of wait thread`
  - `sources/qemu`: `df2839b19d qemu: drain legacy rpc queue after vmfd delegation`
  - `sources/qemu`: `fd2f4402f8 qemu: route irqline updates through vmfd ioctl`
  - `sources/qemu`: `135f743dc3 qemu/sel4: skip rpc mapping in direct mode`
  - `sources/qemu`: `43519d1ae3 qemu/sel4: drop legacy rpc compatibility path`
- matching CAmkES/VMM history:
  - `projects/virtioso-camkes-vm`: `3101192 docs: record direct delegation scaffold progress`
  - `projects/virtioso-camkes-vm`: `09077e3 docs: record direct delegation default validation`

Architectural role:

- Moves selected backend handling from implicit shared RPC forwarding toward an
  explicit VM-fd request/response interface.
- Allows userspace to `poll()` the VM fd for delegated work.
- Provides a cleaner integration point for backend device models that should
  not depend on the old queue shape.

Rewrite note:

- Defer this topic. The old direct-delegation history is entangled with direct
  MMIO slots, backend mailbox work, and QEMU queue removal.
- Reconsider only if a current QEMU/backend requirement needs VM-fd delegated
  work. If so, design the UAPI from that requirement and keep it independent of
  generation-based shared-memory slots.

### 9. Backend Mailbox and Generic Backend Request Channel

The range adds a generic backend mailbox inside the control window, after the
normal RPC iobuf. DT validation explicitly sizes the control region for:

```text
rpcmsg_iobuf_t + virtioso_backend_mailbox_t + virtioso_control_mailbox_t
```

Evidence:

- `sources/kmod-sel4-virt/transport/sel4_dt.c`
- `sources/kmod-sel4-virt/sel4_core.c`
- `sources/kmod-sel4-virt/include/uapi/sel4/sel4_virt.h`
- commits include:
  - `7cbe0ae uapi: define generic backend request channel`
  - `05265bc dt: validate backend mailbox control window`
  - `878d176 dt: fix backend mailbox probe format`
  - `b612bbd dt: process async requests from backend mailbox`
  - `a5f1eb8 backend: ring existing dt doorbell on completion`
  - `8605fe9 rpc: publish dt control ops via mailbox`
- matching contract history:
  - `sources/virtioso-contracts`: `9eb8025 contracts: define backend mailbox slots`
  - `sources/virtioso-contracts`: `baf6bb0 contracts: add mailbox slot helpers`
  - `sources/virtioso-contracts`: `77e19e7 rpc: add fixed-slot control mailbox`
- matching VMM-side history:
  - `projects/virtioso-camkes-vm`: `5a551fa rpc: publish async requests through backend mailbox`
  - `projects/virtioso-camkes-vm`: `74a0d35 rpc: bridge backend completions in device vmm`
  - `projects/virtioso-camkes-vm`: `ee1527f rpc: bridge blocking emulation through backend mailbox`
  - `projects/virtioso-camkes-vm`: `7555e72 rpc: drain dt control ops from mailbox`
- matching QEMU history:
  - `sources/qemu`: `111100bf16 sel4: use generic backend request ioctls`

Architectural role:

- Generalizes backend requests beyond the older MMIO-only naming.
- Lets DT-provided backends publish asynchronous work through a shared mailbox.
- Provides the bridge between static DT transport and explicit VM-fd backend
  delegation.

Rewrite note:

- This historical topic is rejected for replay as-is. See the backend mailbox
  decision above.
- Do not reintroduce `generation`, mailbox sync/cache tracing, or the old
  control-window mailbox layout without a fresh requirement.

### 10. Compatibility, Cache, and Correctness Fixes

The range includes smaller but important compatibility/correctness changes:

- `ioeventfd` datamatch width relaxation
- legacy MMIO compatibility callers
- MMIO request token propagation
- event BAR cacheability changes
- request iobuf cache sync and resync
- separation of MMIO forward from error paths

Evidence commits include:

- `d37d4a0 kmod-sel4-virt: relax ioeventfd datamatch width checks`
- `854b404 sel4/rpc: carry MMIO request token in mr3`
- `3474045 sel4: make event bar uncached and DT-sized`
- `32d1906 kmod-sel4-virt: sync cacheable control iobuf on vm0`
- `78f694a kmod-sel4-virt: resync request iobuf after dequeue`
- `99d8264 kmod-sel4-virt: separate MMIO forward from error`
- `23f56e2 kmod: fix legacy mmio compatibility callers`

Architectural role:

- These are not a single feature, but they protect compatibility while the
  transport and request handling model changes.
- Several of them are likely required for the larger topics to behave correctly
  on cacheable control memory.

Rewrite note:

- Do not batch all of these at the end.
- Pull each fix next to the topic that needs it, unless it is independently
  useful and testable.
- Drop cache/shareability workaround commits by default. The old event-BAR and
  RPC/cache-sync changes belong to the resolved Orin AGX shareability bug
  investigation unless a new, separate correctness issue is proven.

### 11. QEMU seL4 Accelerator Consumers

The QEMU branch is not just a test consumer. It owns the userspace side of the
new VM-fd behavior, trace shard initialization, seL4 accelerator MMIO handling,
PCI config behavior, and virtio/physmem tracepoints.

Evidence:

- `sources/qemu/accel/sel4/sel4-all.c`
- `sources/qemu/accel/sel4/sel4-vpci.c`
- `sources/qemu/include/sysemu/sel4.h`
- relevant commits include:
  - `88fc023aa3 qemu: add direct delegation vmfd handler scaffold`
  - `5d969a1550 qemu: use vmfd handlers instead of wait thread`
  - `111100bf16 sel4: use generic backend request ioctls`
  - `409a01f112 sel4: return all-ones for absent pci config reads`
  - `9a95f04d54 sel4: map backend PCI config by devfn slot`

Rewrite note:

- Do not replay QEMU consumers for rejected mailbox/direct-slot protocols.
- Retain QEMU trace tooling only where it consumes the trace framework, not old
  event taxonomies or cache probes.
- Any future QEMU VM-fd/direct-delegation work needs its own fresh UAPI design
  and validation plan.

### 12. Yocto and Image Integration

The runtime path depends on image recipes installing the right modules,
contracts, helpers, and QEMU build. The layer history contains local-source
recipes and package wiring for kmod, QEMU, `virtioso-contracts`, and
`kmod-vio-trace`.

Evidence:

- `vm-images/virtioso-yocto-layers/meta-virtioso-sel4/recipes-devtools/virtioso-contracts/virtioso-contracts.bb`
- `vm-images/virtioso-yocto-layers/meta-virtioso-sel4/recipes-kernel/kernel-module-sel4-virt/kernel-module-sel4-virt.bb`
- `vm-images/virtioso-yocto-layers/meta-virtioso-sel4/recipes-devtools/qemu/qemu_%.bbappend`
- relevant commits include:
  - `de2b2af trace(contracts): add yocto recipe and deps for shared contracts`
  - `2e99045 yocto: install backend mailbox contract header`
  - `0e69091 yocto: install control mailbox header`
  - `8d8b3aa yocto(trace): retarget trace module bindings to canonical kmod-vio-trace`
  - `11f7fe3 yocto(vio-trace): load canonical vio_trace module and emit modprobe diagnostics`

Rewrite note:

- Treat Yocto as part of the validation surface. A host build of kmod or QEMU is
  not enough if the VM images still package old headers/modules.
- Keep local-source cleanliness guards in place because this workflow consumes
  `sources/qemu`, `sources/kmod-sel4-virt`, and `sources/virtioso-contracts`.
- For trace framework extraction, package only the minimal trace contract,
  `kmod-vio-trace`, and tools that consume the retained binary/shard format.

### 13. seL4-Side VMM and Platform Support

The CAmkES and `sel4_projects_libs` histories contain pieces that are not Linux
kmod features but are required for end-to-end behavior:

- CAmkES connector shape and generated DT/reserved-memory layout
- VMM-side backend mailbox publishing and completion bridging
- cross-VM connector BAR modeling
- cacheability and stage-2 mapping behavior
- GICv3 and x86 interrupt/PCI support needed by target platforms
- trace syscall wrappers in `sources/sel4-linux-kernel-support`

Evidence:

- `projects/virtioso-camkes-vm/apps/Arm/vm_qemu_virtio/`
- `projects/virtioso-camkes-vm/src/`
- `projects/sel4_projects_libs/libsel4vmmplatsupport/src/drivers/cross_vm_connection.c`
- `projects/sel4_projects_libs/libsel4vm/src/guest_memory.c`
- `sources/sel4-linux-kernel-support`: `4b369f4 aarch64: append dedicated trace syscall wrappers`

Rewrite note:

- Do not replay VMM-side mailbox changes unless a new no-generation async design
  is explicitly chosen.
- Keep broad platform/debug history out of the minimal rewrite unless a commit
  directly supports the kmod/QEMU contract under test.

## Proposed Rewrite Order

1. Rebase all participating repos to their current intended baselines and record
   the exact starting SHA per repo.
2. Ensure each participating repo is on top of its intended `virtioso-next`
   branch before creating rewrite commits; side-branch histories are evidence,
   not the final commit base.
3. Inventory common copied/mirrored headers across kmod, QEMU, and seL4 VMM
   code, then extract the unchanged shared baseline into
   `sources/virtioso-contracts`.
4. Switch kmod, QEMU, VMM, and Yocto build inputs to consume that
   `virtioso-contracts` baseline, with no behavior change.
5. Validate the contracts-only baseline before adding new feature contracts.
6. Land the single-device event/data/control BAR contract across CAmkES/VMM and
   kmod PCI.
7. Land transport selection skeleton in kmod with PCI still working.
8. Add DT backend discovery using the same three-region model and matching
   CAmkES generated DT/reserved-memory output.
9. Explicitly drop the old RPC/cache-sync workaround family unless a fresh
   non-shareability bug is proven.
10. Explicitly reject old generation-based slot transports: backend mailbox and
    direct MMIO slots.
11. Extract the trace framework only:
    - minimal trace record/header/source identity contract;
    - `kmod-vio-trace` shard provider;
    - CAmkES DT/reserved-memory shard wiring;
    - binary transfer and timeline tooling if independent of old event IDs.
12. Add trace shard bridge UAPI only if the framework extraction needs explicit
    `/dev/sel4`-mediated shard export.
13. Add new event IDs only with concrete retained producers. Do not bulk replay
    old cache, mailbox, MMIO-slot, or queue-visibility phase IDs.
14. Update Yocto image integration for retained contracts/modules/tools.
15. Revisit QEMU VM-fd/direct-delegation work only if a current feature or
    benchmark proves the need; design it without generation-based shared slots.
16. Remove temporary proof/debug markers or gate them clearly.

## Validation Slices

Each topic should have a small validation target before the next topic lands.

- Build validation:
  - contracts-only baseline builds in kmod, QEMU, and seL4 VMM consumers before
    feature changes start.
  - clean `make kmod-sel4-virt` through the canonical workspace-root entrypoint.
  - full `make vm_qemu_virtio` when the module is integrated into the VM image.
  - QEMU accelerator build with the selected QEMU branch.
  - Yocto image rebuild that proves packaged headers match source contracts.

- PCI compatibility validation:
  - backend selector can force `backend=pci`.
  - `/dev/sel4` VM creation still exposes RAM, control/iobuf, and event maps.
  - `ioeventfd` and `irqfd` behavior still works for the existing PCI path.

- DT validation:
  - backend selector can force `backend=dt`.
  - DT probe rejects undersized control windows.
  - DT probe creates the same logical memory maps as PCI.

- Trace framework validation:
  - minimal trace contracts compile in kernel and non-kernel consumers.
  - `kmod-vio-trace` probes DT-described shards by explicit tuple: VM id,
    execution domain, driver VM id, flags.
  - shard mmap rejects invalid offsets and unaligned memory.
  - debugfs fallback exposes the selected shard without requiring old event IDs.
  - binary transfer tooling can parse retained VMM/guest trace buffers without
    mailbox, MMIO-slot, or cache phase IDs.
  - merged trace output preserves ordering/source identity for retained
    producers, even if the event taxonomy is initially small.

- Deferred direct-delegation validation, only if revived:
  - disabled mode preserves legacy forwarding.
  - enabled mode makes VM fd readable for delegated requests.
  - completion is matched without `generation`.
  - QEMU can consume VM-fd delegated requests without depending on old shared
    slot protocols.

## Risks and Open Questions

- The current post-range history mixes behavior, tracing, cache policy, and
  protocol changes. The rewrite should intentionally separate these so failures
  can be attributed.
- DT and PCI should share the logical transport contract, but not necessarily
  identical probe code. The shared boundary should be the `sel4_vmm` maps and
  doorbell operations.
- Backend mailbox and direct MMIO slots are rejected for replay as-is. The risk
  is accidental reintroduction through dependent trace/QEMU commits.
- Direct delegation changes userspace readiness semantics through VM-fd
  `poll()`. Existing users must keep working if this topic is ever revived.
- Trace shard export exposes physical shared memory to userspace. The rewrite
  should preserve strict validation of VM id, execution domain, driver VM id,
  offset, size, and page alignment.
- Old trace event IDs encode old hypotheses. Replaying them without current
  producers would create a misleading observability contract.
- Some supporting branches contain broad platform/debug history. The rewrite
  should cherry-pick only contract-critical slices and avoid absorbing unrelated
  GIC, x86, elfloader, or cache-debug experiments unless validation shows they
  are required.
