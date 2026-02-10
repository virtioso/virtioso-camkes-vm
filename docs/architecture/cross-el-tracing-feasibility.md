# Cross-EL Tracing Feasibility (ARM64, seL4 EL2 or EL1)

## Status and Intent

This document analyzes feasibility and architecture options for a low-overhead
tracing framework that correlates events across execution levels.

The goal is to define a decision-ready design direction without starting code
implementation yet.

This document is architecture/design guidance only. Operational command
authority and build/test policy remain in the canonical SSOT set:

1. `projects/virtioso-camkes-vm/AGENTS.md`
2. `docs/agents/task-router.md`
3. `docs/agents/build-test-runbook.md`
4. `docs/agents/autopilot-testing-policy.md`
5. `docs/agents/preflight-policy.md`
6. `docs/agents/repo-topology-policy.md`
7. `docs/agents/example-workflows-fastpath.md`

## Primary Requirements

1. Support tracing across privilege levels with minimal runtime overhead.
2. Avoid `HVC` and `SVC` in producer hot paths for higher-EL collection.
3. Keep traces in separate source-local buffers, then merge chronologically.
4. Preserve causality across domains using a common timestamp model.
5. Support two operating modes:
   - Virtualization mode (primary): seL4 at EL2, guest Linux at EL1/EL0.
   - Native mode: seL4 at EL1 with only native seL4 user threads at EL0.

## Scope and Non-Goals

### In Scope

- ARM64 platforms.
- seL4 running at EL2 (hypervisor mode) or EL1 (native mode).
- Cross-source merge tooling and metadata design.
- Feasibility of guest EL0 tracing via Linux tracing stack integration.

### Out of Scope

- Full implementation details and code changes.
- Non-ARM64 platforms.
- Platforms that cannot provide end-to-end `CNTPCT` access for required
  producer domains in the selected mode.

## Current Baseline (Repository Grounding)

### EL2/Kernel Tracing

- Existing seL4 ftrace infrastructure records compact marker streams and exports
  binary data with indexed post-processing support.
- Current ftrace marker families already include context/scheduler/vCPU events,
  but not a complete cross-domain time-sync schema.

### Existing Tooling

- Existing decode/index/query tools can be extended instead of replaced:
  - `kernel/tools/decode_ftrace_binary.py`
  - `kernel/tools/ftrace_to_indexed.py`
  - `kernel/tools/ftrace_indexed.py`

### Native VMM/User-Side Infrastructure

- VMM-side tracing/log-buffer related plumbing exists in
  `projects/virtioso-camkes-vm/src/camkes/modules/trace.c`.

### Timer/Counter Context

- seL4 already supports exporting architectural timer access to lower ELs via
  kernel configuration, including physical counter export.
- Linux controls EL0 timer counter access through `CNTKCTL_EL1`; default guest
  behavior may disable EL0 physical-counter reads unless explicitly enabled.
- Existing PMU-related patches are relevant for PMU/ftrace-clock use cases but
  are separate from the architectural timer access contract.
- Platform support is ARM64-generic; Orin AGX and RPi4 are implementation
  targets, not architecture constraints.

### Guest Linux Tracing Substrate

- Linux tracefs/ftrace stack with selectable tracing clocks exists in-tree.
- This enables a low-friction EL0 guest path via native Linux tracing facilities
  instead of new syscall/hypercall-heavy producer paths.

## Operating Modes

## Mode A: Virtualization (Primary Use Case)

- seL4 kernel at EL2.
- Guest Linux kernel at EL1 in VCPU context.
- Guest userspace at EL0.
- Optional native seL4 EL0 threads (for example VMM) also traced.

### Expected Sources

1. EL2 source buffer (kernel/global or per-core shard).
2. EL1 guest source buffer(s), at minimum per-VM (optional per-vCPU shard).
3. EL0 guest stream(s), primarily from Linux tracing stack per VM.
4. EL0 native seL4 stream(s), for example per-VMM/component.

## Mode B: Native (No Guest Virtualization)

- seL4 kernel at EL1.
- Only native seL4 EL0 threads.
- No guest EL1/EL0 sources, no vCPU offset mapping stage.

### Expected Sources

1. EL1 kernel buffer(s).
2. EL0 native per-component/per-thread buffers.

## Feasibility Assessment by Domain

## EL2 (or EL1 in native mode)

Feasibility: High.

- Kernel-local writes into compact buffers are proven.
- Additional metadata markers (time anchors and source IDs) are feasible with
  bounded overhead.

## EL1 Guest Kernel (virtualization mode)

Feasibility: High.

- Per-VM/per-vCPU buffering is feasible.
- Timestamp feasibility is high when EL1 has direct physical counter read
  access (`CNTPCT`).

## EL0 Native seL4 Threads

Feasibility: High.

- Feasible with direct counter reads and lock-free append to shared/user buffers.
- No `HVC`/`SVC` is required in producer path.

## EL0 Guest Linux Userspace

Feasibility: Medium-High (recommended via Linux tracing stack).

- Recommended primary path: Linux tracefs/ftrace/perf/user-events stack, then
  bridge out as merged-source input.
- This avoids introducing a custom userspace ABI first and reduces kernel
  churn, while preserving flexibility.

## Timestamp and Clock Correlation Model

## Canonical Time Domain

Use one canonical merge timeline in architectural physical counter units
(`CNTPCT` domain).

## Virtualization Mode Mapping

No `CNTVCT` reconstruction path is defined in this architecture revision.

Requirements:

1. EL2/EL1/EL0 producers that participate in cross-source chronology must emit
   timestamps from `CNTPCT` domain.
2. seL4 must enable physical counter export according to mode policy.
3. Guest Linux must explicitly allow EL0 physical counter access where guest
   EL0 tracing is in scope.
4. Any source that cannot emit `CNTPCT`-domain timestamps is unsupported for
   this framework revision.

## Native Mode Mapping

No guest mapping stage is needed. Kernel/user events share the same
`CNTPCT`-domain timestamp contract directly.

## Why Separate Buffers Are Still Correct

Separate buffers are recommended and feasible because:

1. Producer locality lowers lock contention and cacheline bouncing.
2. Isolation and ownership boundaries are explicit.
3. Loss/backpressure can be handled per source.
4. Offline merge can reconstruct global order when timestamps and sequence
   metadata are sufficient.

## Source Formats and Analysis Ingress Envelope

Collection-time format is source-native. A single on-target record format is
not required.

Collection-time rule:

1. seL4 kernel tracing keeps native seL4 ftrace format.
2. Linux kernel/userspace tracing keeps Linux-native formats
   (for example tracefs/ftrace/perf/user-events).
3. Native seL4 EL0 tracing (including VMM components) keeps its native
   tracebuffer/record format.
4. QEMU tracing keeps QEMU-native trace format.

Analysis-ingress rule:

1. Each source decoder maps native events into a common correlation envelope
   before merge.
2. Envelope fields are intentionally minimal:
   - `source_type` and provenance/trust domain
   - `source_id` / shard identifier (`vmid`, `vcpu`, `thread`, component)
   - `source_subtype` (for example `vmm`, `guest-kernel`, `guest-user`,
     `qemu`)
   - `local_sequence`
   - `timestamp_domain` and `local_timestamp`
   - `generation_id`
   - disarm-boundary status (`DISARM`, `NO_DISARM_MARKER`,
     `OVERFLOW_SUSPECTED`)
3. Native payload remains source-specific and is not forced into one producer
   binary schema.

## Merge Strategy (Chronological Reconstruction)

Use a deterministic k-way merge over normalized timestamps from the
analysis-ingress envelope.

Tie-break order:

1. normalized timestamp
2. source priority (for exact-equal cycles)
3. local sequence number

Output targets:

1. merged machine format (indexed/event-typed)
2. human timeline view for debugging

## Overhead Considerations

Low-overhead design principles:

1. Single-producer append paths where possible.
2. Preserve source-native payload formats in producer paths.
3. No blocking calls in producer path.
4. No `HVC`/`SVC` in hot event emission paths.
5. Keep expensive correlation and merge work offline.
6. Default producer topology on SMP is per-shard single-writer append
   (per-CPU/per-vCPU/per-thread), not shared multi-producer hot paths.
7. Shared MPMC queues are reserved for low-rate control and transport events,
   not primary tracepoint emission.

## SMP Implications and Architecture Direction

### EL2/EL1 Kernel-Domain SMP

1. Prefer per-CPU shards for kernel-domain producers to avoid shared-buffer
   lock contention and cacheline bounce.
2. Each shard should maintain local sequence ordering and source metadata.
3. Disarm marker semantics are per-shard during dump, not only per logical
   context.

### EL1 Guest Kernel SMP (Virtualization Mode)

1. Prefer per-vCPU shards for guest kernel trace sources.
2. Ensure each shard emits `CNTPCT`-domain timestamps under SMP scheduling.
3. Shared per-VM buffers are acceptable only if multi-producer overhead is
   proven acceptable for target trace rates.

### EL0 Native seL4 SMP

1. Use per-thread source identifiers and local sequence counters.
2. Preserve deterministic merge ordering with
   `(normalized_timestamp, source_id, local_sequence)` tie-break semantics.
3. Armed/generation control state visibility must use explicit acquire/release
   memory-ordering semantics across cores.

### EL0 Guest Linux SMP

1. Reuse Linux tracing stack per-CPU producer behavior and bridge it as guest
   EL0 sources.
2. Preserve provenance metadata (`host`, `guest-kernel`, `guest-user`) in
   merged output so trust boundaries remain explicit.

### Dump/Disarm under SMP

1. Keep runtime dump path non-blocking; do not wait for global quiesce
   acknowledgments.
2. For each selected shard, attempt append of `DISARM` marker before disarm.
3. Decoder truncates each shard stream at first disarm marker for selected
   generation.
4. Missing disarm marker is reported as `NO_DISARM_MARKER`; if overflow/drop
   metadata supports it, analysis may additionally label
   `OVERFLOW_SUSPECTED`.

## Key Risks and Gaps

1. If `CNTPCT` access is not correctly enabled in a producer domain,
   cross-domain ordering cannot be guaranteed.
2. Buffer overrun policy must be explicit (drop-old/drop-new + counters).
3. Guest-provided trace data has trust boundaries and should be marked as such.
4. Schema versioning is required before rollout across tools and sources.
5. Multi-source merge toolchain is not yet integrated end-to-end.
6. Enabling lower-EL timing access is a known timing-channel risk and must be
   treated as debug/benchmark-only configuration.

## Recommended Phased Plan (Still Design Stage)

1. Define common analysis-ingress envelope and sync-marker mapping schema.
2. Validate native mode first (EL1+EL0) with deterministic merge.
3. Add virtualization anchor schema and EL2+EL1 correlation.
4. Add guest EL0 Linux tracing bridge path.
5. Validate overhead and chronology accuracy, then freeze format.

## Acceptance Criteria for the Planned Framework

1. Works in both operating modes from a single architecture.
2. Virtualization mode reconstructs EL2/EL1/EL0 causality in one merged timeline.
3. Native mode reconstructs EL1/EL0 causality without guest dependencies.
4. Producer paths avoid hot-path hypercalls/syscalls for trace emission.
5. Per-source loss statistics and merge-confidence diagnostics are available.

## Arming and Dump Trigger Feasibility

This section defines feasibility and design direction for controlling tracing
state so tracepoints do not emit until explicitly armed, and dump is triggered
through a uniform control path.

### Design Goals

1. Every execution context has C-callable arm/dump control functions.
2. Tracing emission is disabled by default unless armed by policy.
3. Dump operation must first perform global disarming of selected contexts.
4. External trigger support is planned for Orin AGX and RPi4 via GPIO.
5. Selection of traced contexts must be possible before VM0 Linux is healthy.

### Chosen Direction

The following are provisional defaults pending closure of open design decisions.

1. Context selection is mask-based and configurable.
2. Preferred early selection transport is EFI config file -> loader parse -> BootInfo
   extension consumed by tracing control init.
3. Runtime override path still exists (control API from runtime control plane).
4. External trigger uses two dedicated GPIO pins:
   - ARM pin
   - DUMP pin
5. Virtualization mode GPIO trigger controller lives in VM0 kernel module.
6. Dump semantics are defined normatively in `Dump Ordering Requirement`.

### Why EFI Config + BootInfo

Feasibility is high and operationally practical:

1. Works before guest stack/userland and VM0 service readiness.
2. Avoids rebuild-per-selection workflows.
3. Is deterministic for automated test runs (autopilot can write per-run config).
4. Avoids immediate dependence on UEFI NVRAM semantics and cleanup issues.

### Common Control API (Conceptual v1)

Each context exposes equivalent semantics:

- `int trace_ctrl_arm(uint64_t generation_id);`
- `int trace_ctrl_dump(uint64_t generation_id);`
- `int trace_ctrl_set_mask(uint64_t mask);`
- `int trace_ctrl_get_state(struct trace_ctrl_state *out);`

Proposed `trace_ctrl_state` fields:

- `armed` (bool)
- `mask` (context bitmask)
- `generation_id`
- `dump_in_progress`
- `last_error`

### Trace Control Page Architecture (4 KiB, v1 Direction)

This section defines how armed/disarmed state and future control flags are
published to producers with minimal overhead.

Normative model:

1. seL4 control authority is the only writer of control-page state.
2. Producer contexts consume control-page state via read-only mappings.
3. Allocate one 4 KiB control page per producer domain, not per thread.
4. v1 uses one page per VM for guest EL1+EL0 (VM-global arming), with optional
   per-vCPU extension in later revision.

Allocation and capability distribution:

1. CapDL loader allocates control-page frames and emits corresponding caps.
2. Authority/control components receive write-capable mappings only for domains
   they govern.
3. Producer domains receive read-only mappings to their assigned page.

Mapping by execution context:

1. Native seL4 EL0 threads:
   - map domain page read-only into local VSpace.
2. Guest EL1/vCPU:
   - VMM maps the same backing frame into guest IPA as guest read-only.
3. Guest Linux EL0:
   - guest kernel maps the guest-visible page and exposes read-only user
     mappings (for example controlled `mmap` export).

SMP policy:

1. A domain page is concurrently readable by all CPUs/threads in that domain.
2. VM-global page is valid for SMP guests when arming is VM-wide.
3. Per-vCPU control pages are optional future granularity and are not required
   for v1.

Forward-compatibility requirements:

1. Page layout must include `magic`, `version`, and `size`.
2. Layout must include `generation_id` and monotonic update metadata (sequence
   counter or equivalent) for race-safe reader observation.
3. New flags must be additive and backward compatible for existing readers.

v1 control-page semantic fields:

1. `armed` and `generation_id` state visible to producers.
2. Read-only capability hints per domain:
   - `can_arm`
   - `can_dump`
3. Read-only tracepoint-availability mask(s) so domain code can gate emission to
   supported trace classes.
4. Update-sequencing metadata for race-safe multi-core readers.
5. Control-page hints are advisory for producer-side fast checks; seL4 control
   authority remains final and re-validates permission on every control
   request.

### seL4 Syscall ABI Compatibility Policy

The tracing framework must preserve upstream seL4 syscall semantics.

Normative constraints:

1. Do not change semantics, ordering guarantees, return values, error behavior,
   or side effects of any existing seL4 syscall defined in master branch ABI.
2. Do not piggyback tracing control onto existing syscall opcodes or existing
   syscall argument layouts.
3. If kernel entry is required for trace control in future implementation, use
   dedicated tracing-specific syscall interfaces (new syscall IDs/labels and
   dedicated argument contract), or non-syscall control paths where appropriate.
4. Producer hot-path tracepoint emission still must not require `SVC`/`HVC`.
5. Trace control API evolution must be versioned independently from existing
   syscall ABI.

Architecture implications:

1. `trace_ctrl_*` is defined as a logical API contract only; each context can
   bind it to an implementation-specific transport without modifying existing
   syscall semantics.
2. Preferred early control remains EFI config -> BootInfo extension.
3. Runtime control in seL4 domains should prefer existing control-plane shared
   memory/capability channels; dedicated kernel syscalls are optional and only
   for control operations, not tracepoint emission.

### Context Mask Model (v1)

Initial context bits:

- `TRACE_SRC_EL2_KERNEL`
- `TRACE_SRC_EL1_GUEST_KERNEL_ALL`
- `TRACE_SRC_EL0_GUEST_ALL`
- `TRACE_SRC_EL0_NATIVE_ALL`

Per-VM/per-vCPU granularity is reserved for a later revision.

### Dump Ordering Requirement

For `trace_ctrl_dump()`:

1. Enter dump control path.
2. For each selected context `X`, append `DISARM(X)` marker to `X` trace
   buffer, then apply disarm for `X`.
3. Dump/export each context without blocking on per-context acknowledgments.
4. On decode/analysis side, truncate each context at first `DISARM(X)` marker.
5. Apply end policy (halt or continue) for current profile.

This addresses the current risk where dump path itself may generate additional
tracepoints if producers remain armed during dump, while avoiding deadlock from
blocking quiesce waits.

### Per-Context Feasibility

#### EL2 kernel (or EL1 kernel in native mode)

Feasible:

1. Existing reset/finalize hooks already exist.
2. Add explicit armed gating policy and disarm-first dump sequencing.
3. Current finalize path may halt after dump; retain as policy option.

#### EL1 guest kernel (virtualization mode)

Feasible:

1. Selected-source disarm ordering follows `Dump Ordering Requirement`.
2. Arm/dump control can be called through existing control-plane channels.

#### EL0 guest userspace (virtualization mode)

Feasible:

1. Linux tracing stack remains primary source path.
2. Arm/dump maps to enabling/disabling selected Linux trace producers plus
   freeze behavior on global disarm.

#### EL0 native seL4 threads

Feasible:

1. Shared control state allows low-overhead armed checks.
2. Dump ordering and marker semantics follow `Dump Ordering Requirement`.

### External GPIO Trigger Feasibility

#### Common trigger behavior

1. ARM GPIO edge -> `trace_ctrl_arm(next_generation)`.
2. DUMP GPIO edge -> `trace_ctrl_dump(current_generation)`.
3. Apply debounce/coalescing for signal quality and deterministic handling.
4. Record trigger-control events in trace metadata.

#### Orin AGX 64GB devkit

Feasible:

1. Feasible if GPIO controller passthrough is present in platform composition.
2. VM0 kernel module can register GPIO IRQ handlers and invoke control APIs.
3. Pin mapping and polarity are configuration artifacts and must be sourced
   from platform composition docs/config.

#### Raspberry Pi 4

Feasible:

1. Feasible if GPIO passthrough/ownership model is available in composition.
2. Same VM0 kernel-module trigger model applies.
3. Use dedicated ARM/DUMP pins distinct from console wiring when enabled.
4. Exact pin assignment is platform configuration and intentionally not fixed in
   this architecture document.

### Early and Runtime Selection Data Flow

1. One supported automation path writes EFI trace config file per run.
2. EFI loader parses file and publishes trace-control BootInfo extension.
3. Early tracing init consumes default mask/policy.
4. Optional runtime override updates mask/policy via control API.
5. Arm/dump may be initiated by software or GPIO external triggers.

### Failure Handling Requirements

1. Missing/invalid EFI config -> safe defaults + error status.
2. Dump while already dumping -> idempotent error/status response.
3. Missing `DISARM(X)` marker during analysis -> `NO_DISARM_MARKER` status for
   context `X`; optional hint `OVERFLOW_SUSPECTED` if overflow/drop metadata
   supports that inference.
4. Unauthorized arm/dump/control requests:
   - reject deterministically at authority boundary
   - increment per-source counters (`unauth_arm`, `unauth_dump`, ...)
   - avoid unconditional printk flood; optional rate-limited debug logging only
     under debug config.
5. Trigger storms/repeats -> deterministic coalescing/idempotent outcomes.

### Snapshot Export Pipeline (v1 Direction)

This section defines how sealed dumps are exported to analysis-side tooling
without depending on producer liveness.

Normative model:

1. `trace_ctrl_dump()` first seals an immutable snapshot artifact under control
   authority ownership; exporters never read live producer buffers directly.
2. Snapshot metadata must include at least `dump_id`, `generation_id`,
   per-source payload lengths, and integrity/loss diagnostics needed by
   analysis ingress.
3. Export path is transport-ordered:
   - primary: VM0 network export path when available
   - fallback: authority-owned UART export path
4. Control signaling to VM0 exporter uses authority-owned shared memory plus a
   doorbell/notification IRQ; VM0-origin messages are advisory, while state
   transitions remain authority-decided.
5. Export handshake is two-phase with distinct timers:
   - `T_ack`: VM0 acknowledges snapshot-ownership acceptance
   - `T_done`: VM0 reports export completion
6. Timeout policy is deterministic:
   - if `T_ack` or `T_done` expires, transition to UART fallback for same
     `dump_id`
7. Single-winner completion is enforced per `dump_id`:
   - first successful completion wins
   - late completion from other transport is ignored and logged
8. Runtime must remain non-blocking on producer cooperation; stalled EL0/EL1
   producers cannot prevent snapshot export progress.

Architecture implications:

1. VM0 NIC passthrough and VM0 network stack health are optimization
   prerequisites for primary transport, not correctness prerequisites.
2. UART path is mandatory as reliability anchor for stuck/degraded-system
   diagnostics.
3. PC-side ingest (for example autopilot result flow) consumes exported
   snapshot artifacts and performs source-specific decode plus merged timeline
   reconstruction.

### v1 Export Contract (Accepted, 2026-02-10)

#### A) Snapshot Artifact Wire Format

Container structure:

1. Fixed header:
   - `magic = XELTRC1`
   - `version = 1`
   - `header_size`
   - `dump_id`
   - `generation_id`
   - `source_count`
   - `manifest_offset` / `manifest_size`
   - `artifact_crc32c`
2. Manifest (array of source descriptors), one entry per source shard:
   - `source_type`, `source_subtype`, `source_id`, `shard_id`
   - `timestamp_domain` (must be `CNTPCT` for participating sources)
   - `payload_offset`, `payload_size`
   - `flags` (`HAS_DISARM`, `NO_DISARM_MARKER`, `OVERFLOW_SUSPECTED`,
     `TRUNCATED`)
   - `records_exported`, `records_dropped`
   - `source_crc32c`
3. Payload region:
   - raw source-native bytes for each descriptor region
4. Footer:
   - `end_magic`
   - optional duplicate `artifact_crc32c` for tail validation

Validation defaults:

1. If header or manifest validation fails, reject artifact as invalid.
2. If one source payload fails CRC or bounds checks, isolate that source and
   continue ingest for remaining valid sources.

#### B) VM0 Shared-Memory + IRQ Handshake

Shared control block (authority-owned, VM0 read/write window):

1. `protocol_version`
2. `state` (`IDLE`, `OFFERED`, `ACKED`, `DONE`, `FAILED`, `TIMEOUT`)
3. `dump_id`, `generation_id`
4. snapshot descriptor (`offset/len` in authority export region)
5. result fields (`status_code`, `bytes_sent`, `error_code`)
6. monotonic `seq` for race-safe updates

`status_code` set (v1):

1. `OK`
2. `OK_FALLBACK_UART`
3. `ERR_ACK_TIMEOUT`
4. `ERR_DONE_TIMEOUT`
5. `ERR_VM0_FAILED`
6. `ERR_UART_IO`
7. `ERR_ARTIFACT_INVALID`
8. `ERR_PARTIAL_SOURCE_ISOLATION`

`error_code` set (v1):

1. `NONE`
2. `CRC_MISMATCH`
3. `BOUNDS_INVALID`
4. `MANIFEST_INVALID`
5. `SEQ_GAP`
6. `UNSUPPORTED_VERSION`
7. `TRANSPORT_RESET`
8. `INTERNAL`

IRQ semantics:

1. Authority -> VM0 doorbell when state becomes `OFFERED`.
2. VM0 -> authority doorbell when state transitions to `ACKED`, `DONE`, or
   `FAILED`.
3. Authority is final state owner; VM0 updates are requests/status reports.

Timer defaults:

1. `T_ack = 100 ms` (default; configurable later).
2. `T_done = 30 s` (default; configurable later).
3. One VM0 offer retry is allowed before UART fallback.
4. Any timeout or explicit `FAILED` transitions current `dump_id` to UART
   fallback.

#### C) UART Fallback Framing

Frame format (binary, unidirectional, no runtime ACK dependency):

1. `sof` constant
2. `version`
3. `dump_id`
4. `frame_type` (`MANIFEST`, `SOURCE_CHUNK`, `END`, `ABORT`)
5. `frame_seq`
6. `payload_len`
7. `payload`
8. `frame_crc32c`

Defaults:

1. Max payload chunk size: `1024` bytes.
2. `END` frame includes total frame count and terminal status.
3. Decoder can detect gaps/corruption using `frame_seq` and CRC.

#### D) Analysis-Ingress Boundary

1. Ingress begins at artifact/container parse.
2. Mandatory checks before per-source decode:
   - magic/version
   - offsets/sizes in bounds
   - manifest uniqueness and referential validity
   - artifact/source CRC checks
3. Valid sources continue to decode even if some sources are isolated as
   corrupt/missing.

Reject-vs-isolate matrix (v1):

1. Header magic/version invalid -> reject whole artifact.
2. Header/manifest bounds invalid -> reject whole artifact.
3. Duplicate `(source_id, shard_id)` in manifest -> reject whole artifact.
4. Artifact CRC failure -> reject whole artifact.
5. Source payload out of bounds -> isolate that source.
6. Source CRC failure -> isolate that source.
7. Missing `DISARM` marker -> keep source; flag `NO_DISARM_MARKER`.
8. Mixed/invalid timestamp domain in one source -> isolate that source.
9. Unknown optional source subtype -> isolate that source.
10. Unknown required core field -> reject whole artifact.

Completion rule (v1):

1. If at least one source is valid, overall completion is `OK` or
   `OK_FALLBACK_UART`; include `ERR_PARTIAL_SOURCE_ISOLATION` diagnostic when
   any source was isolated.
2. If zero sources are valid, overall completion is `ERR_ARTIFACT_INVALID`.

## Stuck-Producer Resilience and Guaranteed Dump

This section defines architecture rules for the primary failure case where one
or more EL0/EL1 producers are jammed and cannot cooperate at dump time.

### Problem Statement

1. A blocked producer may never acknowledge quiesce.
2. A dump design that waits for per-producer acknowledgments can deadlock.
3. Tracing is often used precisely when such stuck states happen, so dump
   progress must not depend on producer liveness.

### Architecture Direction

1. Dump orchestration must be out-of-band relative to normal producers:
   - virtualization mode: orchestrator anchored in EL2 control plane
   - native mode: orchestrator anchored in seL4 kernel/control plane at EL1
2. Producer cooperation is optional, not required, for dump progress.
3. Runtime path does not block on global quiesce acknowledgments.

### Buffer Ownership and Access Model

1. Buffers are logically control-plane owned and always dump-readable by the
   orchestrator.
2. Producers are append-only writers to their assigned shard while armed.
3. A jammed producer cannot prevent another authority context from exporting
   shard contents.

### Dump Progress Rules (No-Deadlock Contract)

1. On dump request, control transitions to `DUMPING` and disarms selected
   contexts per `Dump Ordering Requirement`.
2. For each selected shard/context, append of `DISARM` marker is attempted
   before disarm.
3. Export proceeds even if marker append fails for some shard.
4. No wait-for-ack path is permitted in runtime dump sequencing.

### Analysis-Side Boundary Semantics

1. Decoder truncates per shard at first disarm marker for selected generation.
2. If marker is absent, shard is flagged `NO_DISARM_MARKER`.
3. If overflow/drop metadata exists, analysis may add
   `OVERFLOW_SUSPECTED` as a stronger explanation.
4. Any deeper inference is analysis-side only and must not block platform dump.

### Concurrent Append Safety (SMP-Compatible)

1. Record publication must use a commit protocol so partially written records
   are never treated as valid during dump.
2. Armed/generation/disarm visibility across cores must follow explicit
   acquire/release ordering contract.
3. This preserves dump correctness even if a producer is stalled mid-path.

### Platform Scope for This Trigger Plan

1. Generic architecture remains ARM64.
2. External GPIO trigger implementation plan is explicitly scoped to:
   - Orin AGX 64GB devkit
   - Raspberry Pi 4
3. Platform passthrough/pin truth must be taken from platform-specific
   composition docs, not duplicated here (for example:
   `docs/platforms/orin-agx/porting/vm-qemu-virtio-orinagx.md`,
   `docs/start-here/running-rpi4.md`).

## High-Level Architecture Focus Areas

This section captures the architecture-first view before low-level API or code
detail.

### 1) Control-Plane Authority Model

Define one logical trace control authority that owns lifecycle transitions and
policy decisions (`DISARMED`, `ARMED`, `DUMPING`).

High-level responsibilities:

1. Select active source mask and generation ID.
2. Coordinate global disarm-before-dump ordering.
3. Publish consistent state to all producer domains.
4. Record control actions as trace metadata for auditability.

Mode split:

1. Virtualization mode: authority is anchored in seL4/host control plane,
   with VM-facing adapters.
2. Native mode: authority remains in seL4 kernel/user control plane only.

### 2) Trigger-Source Abstraction

Treat trigger origins as pluggable sources feeding the same semantic actions:
`ARM` and `DUMP`.

Trigger classes:

1. Software control call (internal C API path).
2. External hardware trigger (GPIO ARM/DUMP edges).
3. Optional automation trigger (test harness profile flow).

Architecture rule:

1. Trigger source decides *when* an action is requested.
2. Control-plane authority decides *whether* transition is allowed and applies
   policy.
3. All accepted/rejected trigger attempts become trace control events.
4. Request authorization mapping is policy/config driven and must not hardcode
   specific VM identities (for example VM0/VM1); any authorized domain may be
   granted `ARM` and/or `DUMP` rights.

### 3) Lifecycle State Machine

Use a small explicit state machine shared across modes:

1. `DISARMED`: tracepoints are effectively no-op.
2. `ARMED`: tracepoints emit into selected buffers.
3. `DUMPING`: control path performs per-context disarm-marker sequence and
   exports data.

Required transitions:

1. `DISARMED -> ARMED` via valid arm trigger.
2. `ARMED -> DUMPING` via dump trigger.
3. `DUMPING -> DISARMED` when dump/export completion policy finishes.
4. Optional `ARMED -> DISARMED` explicit software disarm.

State-machine invariants:

1. For each context `X`, `DISARM(X)` marker append is attempted before disarm
   for `X`.
2. Generation ID changes only on successful arm transition.
3. Re-entrant dump requests are rejected or coalesced deterministically.
4. Control ingress remains bounded; overflow policy for control commands is
   deterministic and does not affect producer hot paths.

### 4) Mode-Specific Architecture View

Detailed mode definitions are canonical in `Operating Modes` above. This
section only lists mode-specific architectural deltas to avoid duplicating mode
topology.

#### Virtualization mode (primary)

1. Time normalization includes guest mapping/anchor reconstruction.
2. Trigger fan-out requires VM-aware adapters to propagate arm/disarm semantics
   into guest-visible tracing controls.

#### Native mode (seL4 EL1 + native EL0 only)

1. No guest mapping stage is required in merge pipeline.
2. Same control semantics and state machine must be preserved to keep one
   framework architecture.

### 5) Ingress and Data-Flow Layers

High-level ingress stages:

1. Early policy ingress:
   - configuration supplied before runtime service readiness
   - consumed during boot to set default mask/policy
2. Runtime override ingress:
   - software control path can update mask/policy after boot
3. Event ingress:
   - producers append to local per-shard buffers while armed
   - hot path stays single-writer per shard on SMP
4. Control ingress:
   - optional MPMC command queue may carry low-rate control events
   - examples: `ARM`, `DUMP`, mask/policy updates
5. Correlation ingress:
   - merge pipeline consumes per-source streams and normalizes timestamps

Data-flow ordering rule:

1. Policy/selection ingress must happen before first arm in deterministic test
   flows.
2. Runtime overrides affect subsequent events only (no retroactive rewrite).
3. Dump ingress enforces global disarm, then snapshot/export, then finalize.

## Urgent Decision Checklist (Dependency-Ordered)

This checklist captures the highest-leverage decisions that gate downstream
architecture closure. Order is normative: later items depend on earlier ones.

### D1) Cross-Context Timestamp Contract (Resolved, 2026-02-10)

Why this is first:

1. Determines analysis-ingress envelope fields and validation.
2. Determines merge ordering correctness under SMP and cross-domain flow.
3. Determines whether source participation is allowed or rejected.

Recommended default:

1. Keep strict `CNTPCT`-domain-only participation for v1.
2. Reject/isolate sources that cannot prove `CNTPCT` domain.

Decisions this unlocks:

1. D4 writer/shard model details and ordering guarantees.
2. D5 dump-boundary confidence labeling behavior.
3. Final closure of `Timestamp/Clock Contract` gap.

Resolution:

1. Accepted: strict `CNTPCT`-domain-only participation is mandatory for all v1
   participating sources (including guest EL0 when enabled).
2. Accepted: sources that cannot prove `CNTPCT` domain are rejected or isolated
   from merged chronology.

### D2) Control-Plane Interface and Authority Routing (Resolved, 2026-02-10)

Why this is second:

1. Fixes how `ARM`/`DUMP`/policy updates enter authority from each execution
   context.
2. Fixes idempotency/coalescing behavior under concurrent requests.
3. Fixes authorization enforcement boundaries.

Recommended default:

1. Keep logical `trace_ctrl_*` API shared across contexts.
2. Keep implementation transport-specific (shared memory/notification/GPIO
   adapters) without changing existing seL4 syscall semantics.
3. Keep authority as the sole state-transition writer.

Decisions this unlocks:

1. D3 control-page ABI field finalization.
2. D5 dump orchestration timers and completion states.
3. Final closure of `Control Authorization and Deterministic Request Handling`
   gap.

Resolution:

1. Accepted: multi-domain initiation is explicitly allowed; any authorized
   domain may request `ARM` and/or `DUMP`.
2. Accepted: control authority is the sole state-transition decider and may
   accept/reject requests per policy.

### D3) 4 KiB Trace-Control Page ABI Freeze (v1) (Resolved, 2026-02-10)

Why this is third:

1. Is the shared contract across seL4 authority, VMM, guest kernel, and guest
   user mappings.
2. Affects CapDL allocation/mapping model and compatibility versioning.
3. Affects producer hot-path armed checks and trace-class gating.

Recommended default:

1. Freeze v1 fields as already directed in
   `Trace Control Page Architecture (4 KiB, v1 Direction)`:
   `magic/version/size`, `armed`, `generation_id`, sequencing metadata,
   `can_arm`, `can_dump`, tracepoint availability masks.
2. Keep one page per producer domain in v1 (VM-global for guest EL1+EL0),
   reserve per-vCPU page extension for later revision.

Decisions this unlocks:

1. Mapping/capability details for native EL0, guest EL1, and guest EL0 export.
2. Producer-side fast-path contract and memory-ordering contract closure.
3. Final closure of remaining control-page parts in `Buffer Ownership and Access
   Control` gap.

Resolution:

1. Accepted: v1 baseline keeps VM-global control page for SMP guests (not
   per-vCPU).
2. Accepted: per-vCPU control-page granularity remains a future extension, not
   a v1 requirement.

### D4) SMP Writer Topology and Overflow Policy Convergence (Resolved, 2026-02-10)

Why this is fourth:

1. Sets hot-path overhead and lock/atomic behavior.
2. Sets what metadata is mandatory for deterministic merge and diagnostics.
3. Sets overflow semantics and confidence labels under stress.

Recommended default:

1. Keep per-shard single-writer append topology (per-CPU/per-vCPU/per-thread).
2. Keep shared MPMC queue usage only for low-rate control/transport, not event
   emission.
3. Keep `drop-new` overflow policy for v1 and require counters/labels at
   analysis ingress.

Decisions this unlocks:

1. Final disarm marker schema fields for SMP-safe truncation.
2. Final mandatory counters/confidence labels and SLO interpretation.
3. Final closure of `Backpressure and Confidence Semantics` gap.

Resolution:

1. Accepted: v1 has no exception path that overrides `drop-new` overflow
   policy.
2. Accepted: per-shard single-writer append remains default producer topology;
   shared MPMC remains limited to low-rate control/transport paths.

### D5) Dump Orchestration and Completion Policy (Resolved, 2026-02-10)

Why this is fifth:

1. Defines reliability behavior for stuck-producer scenarios (primary use case).
2. Defines deterministic completion under primary/fallback transport.
3. Defines end-state behavior after dump (halt/continue policy surface).

Recommended default:

1. Keep disarm-per-context with `DISARM(X)` attempt before disarm, no global
   quiesce wait.
2. Keep snapshot-first export model with VM0 network primary and UART fallback.
3. Keep Kconfig-configurable dump end-policy (`halt-after-dump` vs
   `continue-after-dump`), with runtime/profile override rules finalized later.

Decisions this unlocks:

1. Final timeout ladder (`T_ack`, `T_done`) and single-winner completion.
2. Jammed-producer SLO closure and degraded-pipeline escalation behavior.
3. Final closure of `Export and Ingestion Topology` and
   `Jammed-Producer Service Objectives` gaps.

Resolution:

1. Accepted: runtime dump sequencing must not block on producer acknowledgments,
   even with partial marker loss.
2. Accepted: dump progress remains authority-driven and non-blocking on
   producer liveness.

## Open Design Decisions for Next Planning Iteration

1. Exact BootInfo payload structure and versioning for trace-control extension.
2. Exact EFI config file format (binary vs text) and validation checksum.
3. Final analysis-side policy for `NO_DISARM_MARKER` handling and
   `OVERFLOW_SUSPECTED` inference thresholds.
4. Final shard granularity defaults per context type
   (per-CPU/per-vCPU/per-thread).
5. Exact disarm marker schema fields for SMP-safe truncation
   (`source_id`, `shard_id`, `generation_id`, `local_sequence`).
6. Formal memory-ordering contract for cross-core armed/generation state
   transitions.
7. Exact control-plane ownership/mapping mechanism for dump-readable buffers in
   virtualization and native modes (trace-buffer paths; control-page mapping
   model is defined in `Trace Control Page Architecture (4 KiB, v1 Direction)`).
8. If dedicated seL4 tracing syscalls are introduced, finalize syscall
    namespace allocation, capability checks, and compatibility policy across
    kernel revisions.
9. Resolved: VM0 transport offer retry count is fixed at `1` in v1 (not
   Kconfig-tunable in v1).
10. Mandatory counters/diagnostics and confidence labels for `drop-new`
    backpressure reporting.
11. Target dump-completion and loss SLOs for jammed-producer scenarios.
12. Final per-mode capability contract for `CNTPCT` enablement chain
    (seL4 EL2/EL1 export, guest Linux `CNTKCTL_EL1` policy, guest EL0 usage).
13. Final wording and placement of mandatory "timing-channel danger" warnings
    across architecture docs and implementation-facing docs.
14. Final Kconfig surface for dump end-policy (`halt-after-dump` vs
    `continue-after-dump`) and interaction with profile/runtime overrides.

## Architecture Gaps to Close Before Implementation

The following are blocking architecture gaps identified in review. They must be
resolved in this design before implementation begins.

### 1) Control Authorization and Deterministic Request Handling

Required outcomes:

1. D2 resolved authority routing: any authorized domain may request
   `ARM`/`DUMP`; authority is the sole transition decider.
2. Finalize capability/permission checks and rejection behavior for unauthorized
   requests.
3. Finalize deterministic idempotency/coalescing semantics under repeated/concurrent
   control requests.
4. Security anti-abuse hardening is out-of-scope for this debug/benchmark
   framework revision; reliability-focused determinism is in-scope.

### 2) Export and Ingestion Topology

Required outcomes:

1. v1 contract, status/error sets, and reject-vs-isolate matrix are defined in
   `v1 Export Contract (Accepted, 2026-02-10)`.
2. Finalize terminal status-code to trace-event mapping for implementation
   diagnostics.

### 3) Timestamp/Clock Contract

Required outcomes:

1. D1 resolved the top-level contract: participating sources must be
   `CNTPCT`-domain; non-conforming sources are rejected or isolated.
2. Finalize producer-family enforcement details (Linux guest/QEMU/VMM adapters)
   so `CNTPCT` compliance is machine-verifiable.
3. Finalize timestamp-domain metadata/validation checks at ingest and exact
   reject-vs-isolate policy reporting surface.

### 4) Buffer Ownership and Access Control

Required outcomes:

1. D3 resolved v1 control-page baseline: one page per producer domain with
   VM-global page for SMP guests.
2. Finalize read/write ownership for each shard class.
3. Finalize mapping/capability rules so dump authority is guaranteed while writers
   remain isolated.
4. Define integrity protections against cross-context buffer corruption.
5. Finalize control-page field schema and memory-order contract for authority
   writes and producer reads.

### 5) Backpressure and Confidence Semantics

Required outcomes:

1. D4 resolved v1 overflow baseline: `drop-new` with no v1 overrides.
2. Finalize exact counter semantics and mandatory loss counters/marker
   requirements.
3. Finalize deterministic analysis labels and confidence grades.

### 6) Jammed-Producer Service Objectives

Required outcomes:

1. D5 resolved non-blocking runtime sequencing under stuck-producer conditions.
2. Finalize target maximum dump latency and minimum exported-data guarantees.
3. Finalize timeout/escalation behavior when export pipeline itself is degraded.

### 7) Cross-Mode Counter Capability Policy

Required outcomes:

1. D1 resolved mandatory `CNTPCT` capability for participating producers in
   each mode.
2. Define mode-level capability reporting and explicit unsupported-source
   behavior where capability is missing.
3. Define enablement requirements across layers (seL4 export and guest Linux
   `CNTKCTL_EL1` policy for EL0 tracing).

## Implementation Gate

Normative rule:

1. Implementation must not begin until all items in
   `Architecture Gaps to Close Before Implementation` are resolved and the
   corresponding entries in `Open Design Decisions for Next Planning Iteration`
   are closed with explicit decisions.
2. Any prototype code written before closure is non-authoritative and must not
   be used to claim architecture readiness.

## Consolidation of Existing Tracing Work (Post-Upstream Baselines)

This section captures tracing-related work already done in local branches after
upstream baseline revisions, and defines how to consolidate it into the new
cross-EL framework with minimal integration cost.

### Baseline Definition

1. For manifest-managed repositories, baseline is the pinned revision in
   `.repo/manifests/combined.xml`.
2. For repos not pinned there, baseline is merge-base against their upstream
   tracking branch.
3. Consolidation target is: from those baselines, apply a minimal patch stack
   that brings in the new framework and keeps integration predictable.

### Inventory Summary (Tracing-Related Deltas)

#### `kernel` (baseline from manifest)

1. Existing local tracing work already includes:
   - ftrace buffer/function infrastructure
   - scheduler/vCPU/reset marker families
   - binary decode/index/query tooling
2. These artifacts are the strongest base for EL2/EL1-kernel domain tracing and
   should be treated as primary reusable substrate.

#### `projects/virtioso-camkes-vm` (baseline from upstream merge-base)

1. Existing local tracing work includes:
   - `hyp_ftrace` MMIO control adapter in VMM
   - VMM-side tracebuffer/ramoops shared-memory plumbing
2. These components should be consolidated as control-plane adapters and memory
   ownership/plumbing helpers, not as the final canonical architecture API.

#### `vm-images/virtioso-yocto-layers` (baseline from upstream merge-base)

1. Existing local tracing work includes:
   - Linux patch integration for hyp-ftrace/platform tracing support
   - PMU EL0 access and PMU-based ftrace clock patches (Raspberry Pi dynamic
     layer)
   - trace support/collection packaging recipes
2. These are integration/packaging assets and should remain optional, layered
   above core framework semantics.

Consolidation note:

1. PMU access/clock patches are not substitutes for the framework's
   `CNTPCT`-domain contract.
2. Add a dedicated Linux integration patch path (per platform kernel tree) for
   guest EL0 `CNTPCT` enablement via `CNTKCTL_EL1` when guest EL0 tracing is
   enabled.

#### Other inspected source repos

1. No committed tracing-specific deltas were identified for:
   - `sources/kmod-sel4-virt`
   - `sources/qemu`
   - `sources/sel4-linux-kernel-support`
2. They are not required as mandatory consolidation inputs for framework v1.

### Consolidation Strategy (Minimal Integration Patch Stack)

Use ordered bundles with explicit dependencies:

1. Bundle A: Kernel tracing core (`kernel`)
   - common event/marker schema for kernel-origin events
   - stable export format and decode/index tooling baseline
2. Bundle B: Control adapters (`projects/virtioso-camkes-vm`)
   - context control fan-out and VMM integration path
   - buffer mapping/ownership implementation that matches this document
3. Bundle C: Platform and image integration (`vm-images/virtioso-yocto-layers`)
   - Linux/Yocto enablement, drivers, and packaging
   - platform-specific knobs (Orin AGX, RPi4)
4. Bundle D: Documentation/runbooks
   - architecture docs, operational notes, migration guidance

Rules:

1. A+B define framework semantics and must remain platform-generic.
2. C is optional and must not redefine architecture semantics.
3. D describes but does not authorize operational commands outside SSOT set.

### Compatibility and Migration Direction

1. Preserve existing guest/VMM trigger surfaces (for example `hyp_ftrace`
   arm/dump behavior) as compatibility adapters where needed.
2. Route adapters to the new framework control model so legacy workflows can
   continue while adoption happens.
3. Do not require immediate replacement of existing tooling if schema mapping is
   deterministic and loss/ordering semantics are preserved.

### Syscall Compatibility Constraint During Consolidation

1. Consolidation must honor `seL4 Syscall ABI Compatibility Policy` above.
2. Existing upstream syscall semantics remain unchanged.
3. If kernel entry points are required for tracing control, use dedicated
   tracing-specific interfaces or non-syscall control-plane channels.
4. Producer hot-path emission remains syscall/hypercall free.

### Acceptance Criteria for Consolidated Drop-On-Upstream Flow

1. Starting from baseline revisions, applying bundles A->B yields a functional
   framework core in both virtualization and native modes.
2. Bundle C can be applied independently per platform without changing core
   framework semantics.
3. Merge/decode toolchain can consume consolidated outputs with deterministic
   ordering and explicit diagnostics (`NO_DISARM_MARKER`,
   `OVERFLOW_SUSPECTED`).
4. Integration does not depend on semantic changes to existing seL4 master
   syscalls.
