# Cross-Arch Console Mux CAmkES Architecture Plan

Date: 2026-04-25

Related documents:

- [console-transport-and-routing.md](console-transport-and-routing.md)
- [autopilot-console-source-integration.md](autopilot-console-source-integration.md)
- [console-stream-topology-diagrams.md](console-stream-topology-diagrams.md)
- [../integration/console-router-and-timeline-implementation-plan-2026-04-23.md](../integration/console-router-and-timeline-implementation-plan-2026-04-23.md)

## Progress Tracking

This file is the live implementation tracker for the console-mux architecture
work. Progress should be updated here as slices move from design into code.

Current slice status:

- Slice 1 `Dedicated QEMU mux uplink`: `in_progress`
- Slice 2 `Shared ConsoleMux component`: `pending`
- Slice 3 `Init diagnostics -> mux-facing interface`: `pending`
- Slice 4 `Guest UART sources -> explicit mux inputs`: `pending`
- Slice 5 `Physical UART sink boundary`: `pending`
- Slice 6 `Router/Autopilot stay dumb`: `pending`

Implementation notes:

- 2026-04-25: started Slice 1 implementation in
  [tools/qemu_runner.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/qemu_runner.py:1)
  and added
  [tools/qemu_mux_uplink_bridge.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/qemu_mux_uplink_bridge.py:1)
  to establish a dedicated second-QEMU-uplink mode for binary-framed runs.
- 2026-04-25: verified by smoke test that the remote bundle wrapper emitted for
  a `vm_qemu_virtio`-profile binary includes:
  - `qemu_mux_uplink_bridge.py`
  - `console-mux.sock`
  - `socket,id=virtioso_mux`
- 2026-04-25: extended Slice 1 support into the local QEMU path in
  [tools/qemu_runner.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/qemu_runner.py:1)
  so local runs can also append a dedicated QEMU mux uplink socket and route
  it through `qemu_mux_uplink_bridge.py`.

## Scope Update

This architecture must not be tied to x86 only.

Although the immediate problem came from the x86 `vm_qemu_virtio`
investigation, the intended seam is:

- shared across x86 and Arm
- owned in `projects/virtioso-camkes-vm`, not in one app directory
- implemented as a reusable component under
  `projects/virtioso-camkes-vm/components/`
- supported by reusable generation/templates under
  `projects/virtioso-camkes-vm/templates/`

App-specific work should be limited to composition and stream-source binding,
not to maintaining separate mux implementations per architecture.

Additional requirement clarified after the initial draft:

- apps built on top of `virtioso-camkes-vm` and derivatives should support the
  muxer/demuxer implicitly
- app authors should not need to hand-describe stream mappings
- therefore the architecture must distinguish:
  - implicit discovery/generation at build time
  - explicit stream identity at wire/runtime time

## User Question

The current understanding is that the system should have a CAmkES component
that receives raw "UART outputs" from other CAmkES components and muxes them
into one output stream that is written to the real physical UART, which in the
x86 QEMU case means the outer `qemu-system-x86_64` emulated serial port.

That led to these architecture questions:

- is upstream `SerialServer` really the right place for this job?
- should `SerialServer` be left untouched and replaced in this app by a
  dedicated console-mux CAmkES component?
- can `printf`, `ZF_LOG`, and related output be redirected through the
  `set_putchar(...)` seam into that mux instead of writing directly to the
  current host-visible path?
- can source identity come from CAmkES wiring instead of explicit downstream
  heuristics?
- could build artifacts help derive decoding rules instead of relying on
  plaintext matching?

## Answer

Yes: the cleaner architecture is to leave upstream `SerialServer` alone and
introduce a dedicated repo-owned CAmkES console-mux component that is reusable
for both x86 and Arm app families.

The main reasoning is:

- `SerialServer` is a shared human-console multiplexer with interactive
  switching behavior, ANSI color handling, buffering, and compatibility
  assumptions. It is not a clean semantic stream-ownership boundary.
- `set_putchar(...)` is already a viable seam for redirecting `printf` and
  `ZF_LOG` traffic away from the default path.
- CAmkES wiring is the right place to define producer ownership.
- the mux output should still use an explicit framed transport on the wire; the
  decoder should not reconstruct intent from text or from build artifacts.

So the target ownership model should be:

- CAmkES wiring defines which producer a byte stream came from
- a dedicated console-mux component assigns stable stream ids
- the mux emits explicit framed records
- the router only decodes frames and materializes named channels
- Autopilot consumes only post-demux named channels

Refined answer after the “implicit for all apps” requirement:

- stream discovery should be implicit by convention and generation
- stream identity on the wire should remain explicit
- the router/demuxer should preserve source information, not decide whether to
  drop it
- any collapsing or grouped view should happen later, as a consumer or
  presentation choice

## Current State

Verified from code:

- `SerialServer` is presentation-heavy and includes multi-client switching and
  colorized output behavior:
  [projects/global-components/components/SerialServer/src/serial.c](/home/hlyytine/tii-sel4/projects/global-components/components/SerialServer/src/serial.c:1)
- x86 producer framing currently piggybacks on host-visible `putchar_putchar`
  and `guest_putchar_putchar` sinks:
  [projects/vm/components/Init/src/console_frame_transport.c](/home/hlyytine/tii-sel4/projects/vm/components/Init/src/console_frame_transport.c:1)
- `Init` already redirects global output through `set_putchar(...)`:
  [projects/vm/components/Init/src/main.c](/home/hlyytine/tii-sel4/projects/vm/components/Init/src/main.c:635)
- the repo architecture rule is already that producer owns source identity and
  the router should not infer it:
  [console-transport-and-routing.md](console-transport-and-routing.md)

Strong architectural conclusion:

- continuing to adapt `SerialServer` into the authoritative semantic mux for
  x86 `vm_qemu_virtio` will keep pulling compatibility behavior into the wrong
  layer
- a dedicated CAmkES mux component is the better ownership boundary

Additional verified nuance:

- standard VM composition today wires `vm##num.putchar` and
  `vm##num.guest_putchar` as distinct paths:
  [projects/vm/components/VM/configurations/vm.h](/home/hlyytine/tii-sel4/projects/vm/components/VM/configurations/vm.h:93)
- but not all guest-visible output necessarily flows through a dedicated
  `guest_putchar` path
- for example, the Arm PL011 early-debug path currently does:
  `putchar((int)value)` in
  [projects/virtioso-camkes-vm/src/camkes/modules/pl011.c](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/src/camkes/modules/pl011.c:37)
- semantically, those bytes belong to the guest console, but operationally they
  currently flow through the VMM component’s default output path

This is the key architecture constraint:

- sender identity alone is not sufficient to determine semantic stream
  ownership
- some bytes emitted by a VMM component are VMM diagnostics
- some bytes emitted by that same component are guest-console payload

## Recommended Target Architecture

Use a dedicated shared `ConsoleMux` CAmkES component.

Recommended placement:

- component:
  `projects/virtioso-camkes-vm/components/ConsoleMux/`
- templates and generation helpers:
  `projects/virtioso-camkes-vm/templates/`

Recommended usage model:

- x86 and Arm apps both wire into the same component contract
- stream sets may differ by app topology
- component semantics stay shared even when compositions differ

Suggested structure:

1. `Init(vm0)` sends generic diagnostics to a mux input dedicated to VMM
   diagnostics.
2. `Init(vm0)` sends explicit heartbeat/debug traffic to a mux input dedicated
   to `vmm_debug`.
3. VM0 guest UART sends bytes to a mux input dedicated to `driver_vm_console`.
4. VM1 guest UART sends bytes to a mux input dedicated to `user_vm_console`.
5. `ConsoleMux` assigns stable stream ids based on the input endpoint.
6. `ConsoleMux` emits one framed output stream.
7. A physical UART sink writes that framed stream to the outer QEMU serial
   device.
8. `console_router.py` decodes frames only.

This preserves the right separation:

- source identity by topology
- framing by mux
- physical UART ownership by sink
- demux by router
- automation by named channels

## Sender Identity Is Not Enough

The PL011 early-printk case is the clearest example of why “one stream per
CAmkES sender component” is not sufficient.

If stream ownership were inferred only from the identity of the sending CAmkES
component, then:

- `Init(vm0)` diagnostics would land in the VMM stream
- but guest early printk emitted from the PL011 emulation inside that same VMM
  component would also land in the VMM stream

That would be architecturally wrong.

So the mux API must support two concepts:

1. default source identity from the bound sender endpoint
2. explicit semantic override for cases where the sender is acting on behalf of
   another stream owner, such as guest console emulation

## Recommended Emission Model

Use a two-level model.

### Level 1: Default Endpoint-Bound Emission

For the majority of use cases, keep a `putchar`-like convenience path.

Example interpretation:

- a component endpoint is bound by generation to a default stream class
- the sender calls a simple byte-output helper
- the mux infers the default stream from the endpoint binding

This is good for:

- normal VMM diagnostics
- explicit VMM debug
- ordinary component-local output

### Level 2: Explicit Semantic Target Emission

For cases like PL011, the sender must be able to say:

- “I am component X, but these bytes belong to guest console stream Y”

So the shared mux contract should include a richer procedure than raw
`putchar`.

Conceptually:

- `emit_default(byte)` or `emit_default(buf, len)`
- `emit_to_stream(stream_id, buf, len)` or
- `emit_to_class(vm_id, stream_class, buf, len)`

Architecturally, the class-based form is cleaner than hard-coding stream ids in
component logic:

- `emit_to_class(vm_id=0, class=guest_console, ...)`

The mux can translate that to the generated concrete stream id.

This avoids baking numeric stream ids into module code such as PL011.

## Preferred Pattern For PL011-Like Paths

The best refinement after further discussion is not to turn the whole PL011
emulator into a separate CAmkES component.

Instead:

- keep PL011 as an in-VMM emulation module
- keep MMIO fault handling local to the VMM component
- replace direct guest-visible `putchar(...)` emission with a call to a
  per-VM `GuestConsoleSink` CAmkES interface
- let `GuestConsoleSink` forward into `ConsoleMux`
- let `ConsoleMux` infer the stream from the `GuestConsoleSink` instance or
  endpoint identity

This keeps the boundary at the right place:

- emulation stays local to the VMM fault path
- semantic stream ownership is moved to a per-VM sink component
- muxing remains heuristic-free

So the intended flow for PL011-style guest output becomes:

1. guest writes PL011 MMIO
2. the VMM module handles the fault locally
3. instead of `putchar(value)`, it calls `GuestConsoleSink.emit(...)`
4. the per-VM `GuestConsoleSink` forwards the bytes to `ConsoleMux`
5. `ConsoleMux` emits the framed `vmX_guest_console` stream

This achieves the desired property without making PL011 itself a standalone
CAmkES component:

- `vm0` and `vm1` use different `GuestConsoleSink` instances
- the mux can attribute those streams by composition identity
- the PL011 module does not need to know a raw stream id
- cross-VM mixing is prevented by topology rather than by downstream rules

Architecturally, this should be treated as the preferred pattern for
delegated guest-visible output that originates inside a VMM component.

## GuestConsoleSink As A Cross-Arch Component

`GuestConsoleSink` should be architecture-independent.

Its semantic role is stable across architectures:

- “these bytes are guest-visible console output for VM X”

What differs by architecture is only how those bytes are produced, not what
they mean once produced.

This makes `GuestConsoleSink` a good shared component under
`projects/virtioso-camkes-vm/components/` rather than an x86-only or Arm-only
helper.

The x86 side already provides a strong existing seam for this approach:

- standard VM composition distinguishes `putchar` and `guest_putchar`
  in
  [projects/vm/components/VM/configurations/vm.h](/home/hlyytine/tii-sel4/projects/vm/components/VM/configurations/vm.h:93)

So the intended shared interpretation becomes:

- `putchar` -> default VMM diagnostics path
- `guest_putchar` -> per-VM `GuestConsoleSink`
- Arm delegated guest output paths such as PL011 -> same per-VM
  `GuestConsoleSink`
- `GuestConsoleSink` -> shared `ConsoleMux`

This gives both x86 and Arm the same ownership model:

- VMM-local emulation stays local
- guest-visible bytes go through `GuestConsoleSink`
- mux attribution comes from composition identity
- router/demux remains heuristic-free

That also makes x86 the easiest early validation target, because the
`guest_putchar` seam already exists and can be re-bound to the new component
without inventing a new semantic split there.

## What Jinja2 Generation Should Produce

The generated system should therefore not only assign ids to senders.

It should generate:

1. default endpoint-to-stream bindings
- used for ordinary `putchar`-style emission

2. semantic stream-class ids or lookup tables
- used by override cases such as guest emulation paths

Recommended generated concepts:

- VM instance identity
- stream class identity
- resolved concrete stream id

For example:

- `(vm0, guest_console) -> 1`
- `(vm0, vmm_diag) -> 2 or similar`
- `(vm0, vmm_debug) -> 3 or similar`
- `(vm1, guest_console) -> ...`

Then code like PL011 does not need to know a raw stream number. It only needs
to know:

- target VM identity
- semantic class = `guest_console`

## On Runtime-Generated vCPU Threads

Runtime-generated vCPU thread objects on Arm do not invalidate Jinja2-based
stream registry generation.

Reason:

- stream ownership should be bound to stable composition-level identities such
  as VM instance and semantic class
- it should not be bound to transient runtime thread objects

So the generated registry should key off:

- CAmkES component instance
- VM instance number / identity
- semantic stream class

not:

- runtime-created vCPU thread ids

The vCPU threads are execution details, not stream-identity authorities.

## Should Components Be Able To Request Additional Stream IDs?

For primary streams, the answer should be no.

Primary streams should come from the generated registry, because that keeps the
system deterministic and allows the router/manifests to agree by construction.

For extension streams, a narrow controlled escape hatch may be useful.

Recommended model:

- generated registry defines the canonical default streams
- optional reserved extension range exists per VM or per component class
- requesting from that range is explicit and uncommon

This should be treated as an advanced feature, not the baseline design.

Otherwise, the architecture drifts back toward ad hoc stream allocation.

Refined policy after further discussion:

- yes, a CAmkES component should be able to request additional stream ids
- no, this should not replace the implicit base-stream model

The intended hybrid is:

1. implicit base streams by convention
- a standard VM component implicitly gets its default VMM-oriented streams
- each VM instance implicitly gets its canonical guest-console stream

2. declared additional streams by extension
- a component may declare additional semantic streams when the base model is
  insufficient
- this is for cases such as:
  - guest-visible output emitted from within a VMM component
  - rare extra semantic lanes not covered by the default stream classes

3. targeted emission for special paths
- ordinary `printf` / `ZF_LOG` style traffic uses default emission
- special paths such as PL011 or `guest_putchar` use the muxer’s richer
  targeted emit API

This is explicitly meant to support the pattern:

- CAmkES VM component gets an implicit base stream identity
- that same component also has access to an additional guest-console stream
- PL011 / guest-console emulation paths target the guest stream explicitly
- ordinary VMM diagnostics stay on the base/default stream

## Additional Streams: Constraints

The concern is not that additional streams exist. The concern is avoiding an
unstructured free-for-all.

So additional streams should follow these constraints:

- declared, not opportunistically allocated ad hoc
- generated centrally into the stream registry
- exposed to component code as symbolic handles or semantic classes
- not hard-coded as raw numeric stream ids inside module logic

Recommended forms:

- `emit_default(buf, len)`
- `emit_to_class(vm_id, stream_class, buf, len)`
- `emit_to_handle(stream_handle, buf, len)`

Avoid:

- `emit_to_stream(17, buf, len)` in module code

The mux/generation side should translate symbolic identity into the concrete
generated stream id.

## Implicit Vs Explicit

There are two different architecture questions here:

1. how streams are discovered
2. how streams are represented on the wire

Recommended answer:

- discovery: implicit
- wire/runtime identity: explicit

What this means in practice:

- app authors do not hand-write stream maps
- the build/composition machinery generates the stream registry
- every emitted frame still carries an explicit stream id
- the demuxer does not guess, infer, or discard identity

This is the combination that satisfies both requirements:

- implicit support for all standard `virtioso-camkes-vm` apps
- no heuristics in the mux/demux path

## Default Stream Model

The most robust default is not “one guest stream and one everything-else
stream.”

Instead, every standard VM instance should implicitly receive a canonical
default stream set.

Recommended default per VM:

- `guest_console`
- `vmm_diag`
- `vmm_debug`

Optional global/shared classes:

- `control`
- `trace`
- `platform_diag`

So a two-VM app would automatically generate something like:

- `vm0_guest_console`
- `vm0_vmm_diag`
- `vm0_vmm_debug`
- `vm1_guest_console`
- `vm1_vmm_diag`
- `vm1_vmm_debug`

This preserves useful source ownership while staying implicit for app authors.

If a consumer wants a simplified view later, it can derive:

- merged guest-console view
- merged VMM-diag view
- compatibility `tty0`

But the producer contract should preserve the richer identity first.

For implementation, this default model should be interpreted as semantic stream
classes, not only as sender buckets.

That is what allows:

- ordinary VMM diagnostics to use endpoint-default emission
- PL011-style guest emulation paths to target `guest_console` explicitly

## Why Build Artifacts Are Not Enough

Build artifacts can help generate:

- stream-id enums
- port-to-stream tables
- documentation

But they should not replace explicit framing.

The decoder should not need to infer source identity from:

- plaintext prefixes
- formatting conventions
- generated metadata at runtime

The wire format should remain explicit and authoritative.

## CAmkES Jinja2 Generation Option

The CAmkES/Jinja2 path is a real option here, but it should be used to
generate authoritative stream metadata, not to justify decoder heuristics.

Verified enabling facts:

- this repo already adds its own CAmkES template search path via
  `CAmkESAddTemplatesPath(${VIRTIOSO_CAMKES_VM_DIR}/templates)`:
  [virtioso_camkes_vm_helpers.cmake](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/virtioso_camkes_vm_helpers.cmake:19)
- the x86 app still composes the standard VM wiring and imports
  `SerialServer.camkes`:
  [apps/x86/vm_qemu_virtio/vm_qemu_virtio.camkes](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/apps/x86/vm_qemu_virtio/vm_qemu_virtio.camkes:1)
- current VM composition macros already define the serial-related wiring in one
  place:
  [projects/vm/components/VM/configurations/vm.h](/home/hlyytine/tii-sel4/projects/vm/components/VM/configurations/vm.h:93)
- the CAmkES runner supports cross-template state passing and generation-time
  context:
  [camkes-tool/camkes/runner/Context.py](/home/hlyytine/tii-sel4/projects/camkes-tool/camkes/runner/Context.py:141)

So architecturally, yes: Jinja2 templates can generate a stream registry.

### What Should Be Generated

Use Jinja2 to generate two things from the same composition-time source of
truth:

1. a compiled artifact for the producer/mux side
- example:
  - `console_stream_ids.h`
  - `console_stream_registry.c`
- purpose:
  - stable enum values
  - endpoint-to-stream mapping
  - optional source labels for debug

2. a runtime-ingestable artifact for the demux side
- example:
  - `console-stream-registry.json`
  - or an extension of `console-manifest.json`
- purpose:
  - lets the per-session demuxer/router ingest stream definitions without being
    recompiled for each app shape

This gives one generation source, two consumers:

- producer/mux gets compile-time constants
- router/demux gets runtime metadata

### What Should Not Be Generated

Do not generate a system where the demuxer needs to reconstruct stream
ownership by looking at:

- arbitrary component names
- any interface that merely "looks UART-like"
- formatting or plaintext output patterns

The decoder should still operate on explicit framed stream ids.

Generation should help define the stream-id table, not replace the framing.

## Enumeration Strategy

The key design choice is whether enumeration is:

- implicit: “discover everything UART-like automatically”
- explicit: “discover only sources that opt into the console-stream contract”

Recommendation: use explicit opt-in, not broad implicit discovery.

Rationale:

- “UART-style” is an architectural role, not a reliably inferable property
- many components may use `PutChar` or similar interfaces for reasons that do
  not mean “this should become a routed named console stream”
- implicit discovery would over-couple unrelated components to the console
  architecture and make stream identity unstable as apps evolve

So the better pattern is:

1. define a dedicated console-stream source interface or attribute convention
2. only endpoints/components using that convention participate in stream
   registry generation
3. Jinja2 templates enumerate those declared sources

Refined interpretation under the new requirement:

- “explicit opt-in” should not mean per-app handwritten stream maps
- it should mean explicit participation through standard repo conventions and
  standard VM/component macros

So the practical target is:

- implicit for all apps that follow the standard `virtioso-camkes-vm`
  composition model
- explicit only in the generated registry and on-wire stream ids

This gives automatic support without runtime guessing.

Example source classes:

- `vmm_diag`
- `vmm_debug`
- `<vm>_guest_console`
- `<vm>_vmm_diag`
- `<vm>_vmm_debug`
- later:
  - `nested_qemu_control`
  - `trace_control`

## Recommended Template Shape

Prefer repo-local template additions over upstream `camkes-tool` modification
for the first slice.

Recommended structure:

1. add a new dedicated CAmkES interface/connector family for console streams
- this marks sources explicitly
- it avoids mining meaning out of generic `seL4RPCCall` / `PutChar` use

The interface family should support both:

- default endpoint-bound emission
- explicit semantic-target emission for special cases such as guest UART
  emulation done inside a VMM component

2. add a repo-local template that walks the assembly/composition and gathers
all instances/endpoints using that interface

3. generate:
- a header consumed by `ConsoleMux`
- a JSON registry emitted into the build/runtime artifacts

4. have `qemu_runner.py` ingest that generated registry and embed it into
`console-manifest.json` or ship it alongside it

This avoids recompiling the router per app shape while still keeping the stream
table authoritative and build-derived.

## Consumer-Side Collapsing Policy

The demuxer/router should not decide whether to drop source information.

Recommended rule:

- mux and router preserve full source identity
- consumers may choose to collapse or group streams later

Examples:

- Autopilot may watch only `vm0_guest_console` for a login verdict
- a human-facing viewer may present a merged “all VMM diag” stream
- a compatibility export may derive `tty0`

But these are derived views. They must not replace the authoritative
source-tagged stream set.

This is important because once identity is dropped in the demuxer, later tools
cannot recover it.

## Where To Hook Generation

There are three plausible places:

### Option A: Generate From App-Local CAmkES Templates

Use repo-local templates under `projects/virtioso-camkes-vm/templates`.

Pros:

- lowest blast radius
- app-controlled semantics
- no upstream tool patch required

Cons:

- requires a new explicit seam in the app/component definitions

Recommendation:

- best first step

### Option B: Extend Existing VM Macros

Teach the VM composition/configuration macros to emit stream-registry data for
the standard VM serial endpoints.

Pros:

- current VM topology already flows through these macros
- less repeated app boilerplate

Cons:

- risks entangling generic VM macros with app-specific console semantics
- harder to keep the seam narrow if other apps do not want this behavior

Recommendation:

- useful second step once the dedicated console-stream contract is proven

More specifically, this option is attractive because the VM macros already
centralize standard serial-related wiring in
[projects/vm/components/VM/configurations/vm.h](/home/hlyytine/tii-sel4/projects/vm/components/VM/configurations/vm.h:93).

That makes them a plausible long-term place to derive the default implicit
stream classes for all standard VM instances, without requiring each app to
describe them manually.

### Option C: Patch Upstream Generic CAmkES Templates

Modify broader Jinja2 templates in `camkes-tool` or `global-components` to
auto-enumerate generic char-style endpoints.

Pros:

- broadest reach

Cons:

- highest blast radius
- weakest semantic precision
- pulls project-specific console architecture into general upstream machinery

Recommendation:

- do not start here

## Best Architectural Split

The most robust design is:

- CAmkES/Jinja2 generation defines the stream registry
- `ConsoleMux` compiles that registry in
- `qemu_runner.py` exports the same registry for the router as runtime metadata
- `console_router.py` ingests runtime metadata and decodes framed records only

That means:

- mux and demux agree by construction
- the router remains generic and does not need recompilation per app
- the stream table is still explicit and build-derived
- apps get mux/demux support implicitly through standard composition

## Migration Plan Extension

Add these slices to the earlier migration plan.

### Slice 0A: Define An Explicit Console-Stream Source Contract

Goal:

- mark only intended stream-producing endpoints/components as participants in
  the routed console architecture

Touches:

- new interface/connector definitions
- app CAmkES assembly wiring

Validation:

- generated registry includes only intended sources across both x86 and Arm
  apps that opt in

Clarification:

- “opt in” here should primarily happen through standard VM/component
  conventions and shared macros, not through per-app handwritten maps
- the contract should include an override-capable emission API so sender
  identity is not the only source of stream ownership

### Slice 0B: Generate Stream Registry From CAmkES Composition

Goal:

- emit both compile-time and runtime registry artifacts from the same Jinja2
  generation pass

Touches:

- repo-local templates
- possibly helper cmake glue

Outputs:

- `console_stream_ids.h`
- `console_stream_registry.c`
- `console-stream-registry.json`

The generated data should include:

- default endpoint bindings
- semantic stream-class lookup helpers
- VM/class -> concrete stream-id mapping
- declared additional stream mappings for extension cases

Validation:

- generated ids match expected VM0/VM1/debug/control streams for each app
  topology without changing router code

Additional target:

- standard `virtioso-camkes-vm` apps get the registry automatically from
  composition with no per-app stream description file

### Slice 0C: Feed Registry Into Runner/Manifest

Goal:

- make the router ingest generated stream definitions without recompilation

Touches:

- `tools/qemu_runner.py`
- manifest/runtime artifact generation

Validation:

- `console-manifest.json` reflects generated stream map
- router uses that map unchanged

Additional requirement:

- the runtime metadata must preserve full source identity even if some
  consumers later choose a collapsed view

## Migration Plan

### Slice 1: Establish A Dedicated QEMU Mux Uplink Early

Goal:

- move the authoritative mux transport for QEMU-backed targets off the
  contaminated legacy serial/stdout path and onto a dedicated second QEMU
  serial/UART channel

Touches:

- `tools/qemu_runner.py`
- QEMU launch argument generation for x86 and Arm `virt`
- target-side ownership of:
  - x86 COM2
  - Arm `virt` UART1

Validation:

- x86 keeps COM1 as legacy console and exposes a dedicated second serial-backed
  host channel for mux traffic
- Arm `virt` keeps UART0 as legacy console and exposes UART1 as a dedicated
  mux uplink
- router/runtime can read the dedicated mux uplink without consuming merged
  stdout noise

Additional requirement:

- this slice should happen before any deeper investment in “harden framing on
  noisy shared UART” work
- noisy-line hardening remains only a coexistence fallback

### Slice 2: Introduce a Shared ConsoleMux Component

Goal:

- add a shared repo-owned CAmkES component that owns semantic console muxing
  for both x86 and Arm apps

Touches:

- `projects/virtioso-camkes-vm/components/ConsoleMux/`
- `projects/virtioso-camkes-vm/templates/`
- app assemblies that opt into the shared mux

Validation:

- the component can be consumed from both x86 and Arm apps without per-arch
  forks

Additional requirement:

- standard apps can consume it implicitly through shared composition helpers or
  VM macros rather than app-local handwritten stream wiring

### Slice 3: Move Global Init Diagnostics Off SerialServer Semantics

Goal:

- redirect `set_putchar(...)` to the new mux-facing sink instead of directly
  piggybacking on `SerialServer` presentation paths

Touches:

- `projects/vm/components/Init/src/main.c`
- `projects/vm/components/Init/src/console_frame_transport.c`

Validation:

- generic `vm0:` / `vm1:` traffic reaches only the intended control/debug lane

Additional requirement:

- the x86 prototype framing shim should start collapsing here into a real
  mux-facing producer interface, not continue to grow as the long-term owner
  of transport policy

### Slice 4: Move Guest UART Sources To Explicit Mux Inputs

Goal:

- connect VM0 and VM1 guest UART bytes to dedicated mux inputs with stable
  ownership

Touches:

- `projects/vm/components/Init/src/serial.c`
- CAmkES assembly wiring for VM0 and VM1

Validation:

- VM0 guest console appears only on `driver_vm_console`
- VM1 guest console appears only on `user_vm_console`

Extended validation:

- guest-visible output emitted from inside VMM helper modules, such as PL011
  early printk, lands on the guest-console stream rather than the VMM-diag
  stream
- those paths use symbolic targeted emission through the mux API, not raw
  hard-coded numeric stream ids

### Slice 5: Keep Physical UART Sink Separate

Goal:

- preserve a narrow physical UART writer boundary that is not the same thing as
  the semantic mux

Touches:

- either a new minimal sink component
- or a constrained compatibility path into existing serial plumbing

Validation:

- one framed output stream reaches outer QEMU serial unchanged

Additional requirement:

- for QEMU-backed targets, the dedicated second serial/UART uplink established
  in Slice 1 is the preferred implementation path for this boundary

### Slice 6: Keep Router and Autopilot Dumb

Goal:

- router decodes explicit frames only
- Autopilot reads named channels only

Validation:

- no `line_prefixes` dependency for verdict-bearing x86 paths
- no regexes run on merged text

## Validation Path

Canonical validation should include x86 first and Arm second.

X86 path:

1. `make mrproper`
2. `make qemu_x86_64_defconfig`
3. `make vm_qemu_virtio`
4. remote `binary_frames` run through the existing x86 QEMU runner path

Arm path:

1. `make mrproper`
2. `make <arm_defconfig>`
3. `make <arm_vm_target_using_shared_mux>`
4. existing Arm validation path for that app

Success criteria:

- no ANSI/color contamination in verdict-bearing guest-console channels
- no generic `vm0:` / `vm1:` chatter in `vmm_debug`

## Remaining Work Against Current Codebase

The architecture direction is now clear, but the implementation is still in a
transitional prototype state.

The most important remaining work items, verified from the current codebase,
are these.

### 1. The Shared CAmkES Components Do Not Exist Yet

Planned components are still architectural intent only:

- `projects/virtioso-camkes-vm/components/ConsoleMux/`
- `projects/virtioso-camkes-vm/components/GuestConsoleSink/`

They are not present in the tree yet.

This means the current system still expresses source ownership through
existing component-local helper code and legacy serial plumbing instead of
through the intended shared CAmkES boundary.

### 2. VM Composition Macros Still Route Through SerialServer

Both x86 and Arm composition today still bind console-related paths to
upstream `SerialServer`, not to repo-owned mux/sink components.

Verified locations:

- x86:
  [projects/vm/components/VM/configurations/vm.h](/home/hlyytine/tii-sel4/projects/vm/components/VM/configurations/vm.h:93)
- Arm:
  [projects/vm/components/VM_Arm/configurations/vm.h](/home/hlyytine/tii-sel4/projects/vm/components/VM_Arm/configurations/vm.h:147)

So the “implicit for all apps” requirement is not satisfied yet. The current
default composition still gives apps `SerialServer` semantics.

### 3. The Current X86 Producer Is Still A Transitional Init-Side Shim

The current x86 `binary_frames` path is implemented in
[projects/vm/components/Init/src/console_frame_transport.c](/home/hlyytine/tii-sel4/projects/vm/components/Init/src/console_frame_transport.c:1)
as a producer-side framing shim over `putchar_putchar` and
`guest_putchar_putchar`.

It still includes target-policy decisions inside `Init`, including guest-stream
selection by instance name:

- `get_instance_name() == "vm1"` selects the user VM guest stream

That was useful for proving the transport, but it is not the target ownership
model. The target model is for `Init` to emit into mux-facing CAmkES
interfaces, not to hard-code framing policy locally.

### 4. GuestConsoleSink Still Needs To Be Introduced And Reused Cross-Arch

The plan now identifies `GuestConsoleSink` as the preferred boundary for
guest-visible output, but that boundary still needs to be implemented and
wired in.

Cross-arch opportunity:

- x86 already has a natural seam via `guest_putchar`
- Arm delegated guest-output paths such as PL011 should use the same sink

This is the most important shared seam for converging x86 and Arm on one
ownership model.

### 5. Arm Delegated Guest Output Is Still Semantically Wrong Today

The Arm PL011 early-printk path still writes guest-visible bytes through the
default VMM output path:

- [projects/virtioso-camkes-vm/src/camkes/modules/pl011.c](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/src/camkes/modules/pl011.c:37)

This means PL011 remains the clearest proof that sender identity alone is not
sufficient.

Arm virtual console plumbing is also still tied to legacy guest-putchar
semantics:

- [projects/vm/components/VM_Arm/src/modules/vuart_init.c](/home/hlyytine/tii-sel4/projects/vm/components/VM_Arm/src/modules/vuart_init.c:29)

So the Arm side still needs explicit migration to the new sink model.

### 6. Build-Time Stream Registry Generation Does Not Exist Yet

The architecture now depends on:

- implicit discovery/generation at build time
- explicit stream ids on the wire at runtime

But the generation pipeline for that registry has not been implemented yet.

Still missing:

- producer-side generated constants or lookup tables
- runtime-ingestable stream registry metadata for the router/runner

Until that exists, stream identity remains partially defined in handwritten
tooling instead of being generated from the composition source of truth.

### 7. QEMU Runner Still Owns The Channel Inventory

Today the logical channel set for `vm_qemu_virtio` is still declared in
Python inside:

- [tools/qemu_runner.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/qemu_runner.py:668)

That is acceptable for the prototype phase, but it is the wrong long-term
owner. The runner should eventually render manifests from generated metadata,
not define the channel architecture itself.

### 8. Router Presentation Behavior Still Leaks Into Framed Mode

`console_router.py` correctly decodes explicit framed streams, but in framed
mode it still mirrors all received payload bytes to process stdout for
compatibility:

- [tools/console_router.py](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/tools/console_router.py:612)

That is a presentation behavior, not an authoritative routing behavior.

It should eventually become:

- optional compatibility rendering
- or limited to a designated compatibility stream

not part of the authoritative demux path.

### 9. The Physical UART Sink Boundary Still Needs A Concrete Implementation Choice

The plan is clear that semantic muxing and physical UART ownership should be
separate, but that boundary has not yet been finalized in code.

Still to decide:

- a new minimal sink component dedicated to framed uplink output
- or a constrained compatibility path through existing serial plumbing

This matters because the wrong choice would pull legacy `SerialServer`
behavior back into the semantic stream contract.

## Recommended Near-Term Implementation Order

Given the current codebase state, the next implementation slices should be:

1. establish the dedicated second QEMU UART/uplink on x86 and Arm `virt`
2. add shared `GuestConsoleSink` and `ConsoleMux` components
3. rework x86 VM composition so `guest_putchar` flows through
   `GuestConsoleSink`
4. move x86 `Init` diagnostics/debug from direct framed emission to mux-facing
   interfaces
5. add build-time generated stream-registry artifacts
6. make `tools/qemu_runner.py` render manifests from generated registry data
7. migrate Arm PL011 and Arm virtual-console paths to the same sink model
8. reduce router stdout mirroring to compatibility-only behavior

## Current Architectural Bottom Line

The remaining work is now concentrated mainly in:

- shared CAmkES component introduction
- VM macro and composition refactoring
- build-time stream-registry generation
- moving producer ownership out of `Init`-side transport shims and legacy
  `SerialServer` wiring

Autopilot and the router are no longer the primary architectural blockers.
They already have the right general shape:

- router demuxes explicit framed channels
- Autopilot consumes named channels and PTY-backed write endpoints

The harder remaining work is on the producer/composition side, where the
semantic stream boundaries still need to be made real.

## Backward-Compatible Sink Variant

This architecture should preserve a clean backward-compatibility option.

The key rule is that producer-side stream ownership should still be made
explicit before bytes leave the semantic boundary, even when the final sink
chooses to flatten those streams for compatibility.

So the same producer-side contract should support at least two sink backends:

1. authoritative framed backend
- `ConsoleMux`
- preserves stream identity
- emits explicit framed output for router/demux consumers

2. compatibility flattening backend
- a simple compatibility sink such as `ConsoleCompatSink` or
  `ConsolePassthroughSink`
- accepts the same semantically separated inputs
- flattens selected inputs onto one physical UART stream without framing

This is architecturally acceptable because the loss of source identity happens
only at the final sink boundary, not inside producer logic or the demux
contract.

That distinction matters:

- acceptable:
  flatten at the last compatibility sink
- not acceptable:
  flatten in producer APIs
- not acceptable:
  flatten in the semantic mux contract
- not acceptable:
  flatten in the router/demux layer

So backward compatibility does not require preserving `SerialServer` as the
semantic owner. It only requires a sink backend that can intentionally discard
source identity at the edge when a legacy plain-UART output mode is needed.

## Transport Safety And Uncontrolled Writers

There is an important transport-safety concern beyond the CAmkES-side mux
architecture.

Some software can still emit bytes to a physical or emulated UART outside the
scope of the planned mux contract, including for example:

- seL4 kernel debug output
- x86 BIOS or firmware output
- Arm bootloader / ATF / similar early software
- any other legacy or platform-controlled writer that we do not own

Those producers cannot realistically all be made participants in the mux
protocol.

### Architectural Consequence

This means a framed mux protocol running on a shared UART that is also used by
uncontrolled writers must be treated as a coexistence mode, not as the ideal
target architecture.

Framing can be made more robust against contamination, but it cannot make a
shared, uncontended-by-others UART into a truly authoritative transport if
foreign bytes may be interleaved arbitrarily.

### Recommended Target

The preferred long-term architecture is:

- keep legacy firmware / kernel / guest-visible console traffic on the legacy
  console channel
- give `ConsoleMux` a dedicated uncontended uplink that uncontrolled software
  does not own

For QEMU-backed environments, this likely means a separate host-connected
channel rather than reusing the ordinary serial/stdout path.

The key property is not the exact mechanism. The key property is:

- the authoritative mux uplink must be owned only by the mux producer path

### Transitional Safety Measures

If the system must temporarily run the framed mux protocol over a shared UART,
then the parser should still be hardened against contamination.

Recommended transitional measures:

- explicit multi-byte frame magic
- versioned frame header
- channel id and direction
- explicit payload length
- stronger integrity field such as CRC16 or CRC32
- resynchronization logic after garbage

This kind of hardening is useful for coexistence, but it should not be treated
as the final architecture.

### Not Recommended As The Primary Fix

The following should not be treated as the primary architectural answer:

- relying on empirical properties such as “ordinary legacy output is usually
  7-bit ASCII”
- treating a high-bit marker scheme as the main framing contract
- adding retransmission / ACK as if the main problem were cooperative lossy
  transport

The dominant risk here is competing uncontrolled writers, not ordinary
end-to-end packet loss between cooperative endpoints.

So retransmission-style schemes would add complexity without solving the core
ownership problem.

### Practical Rule

Use this rule when evaluating transport variants:

- acceptable transitional mode:
  framed transport over a contaminated shared channel with strong resync and
  integrity checks
- preferred target mode:
  framed transport over a dedicated uncontended channel reserved for
  `ConsoleMux`

This keeps the architecture honest:

- parser hardening is a safety measure
- uncontended transport ownership is the real fix

## Dedicated QEMU Side-Channel Feasibility

The next architectural question is whether QEMU-backed x86 and Arm targets can
use a dedicated host-connected channel for `ConsoleMux`, so that the project
can avoid treating “hardened framing on a noisy shared UART” as the main
answer.

Based on the current codebase and local QEMU sources, the answer is yes for
both QEMU-backed targets, and likely without requiring immediate QEMU source
modifications.

### X86 Q35 / PC Path

The QEMU x86 PC/Q35 machine already initializes multiple ISA serial ports:

- [sources/qemu/hw/i386/pc.c](/home/hlyytine/tii-sel4/sources/qemu/hw/i386/pc.c:1092)
- [sources/qemu/include/hw/char/serial-isa.h](/home/hlyytine/tii-sel4/sources/qemu/include/hw/char/serial-isa.h:31)

`MAX_ISA_SERIAL_PORTS` is 4, so QEMU already supports multiple host-backed ISA
serial endpoints on the x86 machine type.

Architectural implication:

- COM1 can remain the legacy/shared console path
- COM2 (and beyond, if needed) can be reserved for a dedicated mux uplink

This is preferable to continuing to share the authoritative mux transport with
the legacy serial path that BIOS/firmware and other software may also use.

There is also an x86 `isa-debugcon` device available in local QEMU:

- [sources/qemu/hw/char/debugcon.c](/home/hlyytine/tii-sel4/sources/qemu/hw/char/debugcon.c:36)

But `debugcon` should be treated as a secondary option only. It is attractive
as a host-visible side channel, but it is not the best default candidate for
the mux uplink because the mux contract is expected to be bidirectional for
interactive console traffic. A second ISA serial port is the cleaner primary
choice.

### Arm `virt` Path

The QEMU Arm `virt` machine already has support for a second non-secure PL011
UART when `serial_hd(1)` is present:

- [sources/qemu/hw/arm/virt.c](/home/hlyytine/tii-sel4/sources/qemu/hw/arm/virt.c:179)
- [sources/qemu/hw/arm/virt.c](/home/hlyytine/tii-sel4/sources/qemu/hw/arm/virt.c:2400)

The implementation explicitly creates:

- `VIRT_UART0` at `0x09000000`
- `VIRT_UART1` at `0x09040000`

and only `UART0` is set as `/chosen/stdout-path` by default:

- [sources/qemu/hw/arm/virt.c](/home/hlyytine/tii-sel4/sources/qemu/hw/arm/virt.c:939)

Architectural implication:

- UART0 can remain the legacy/default firmware and guest-visible console path
- UART1 can be reserved as a dedicated mux uplink

This is a particularly strong result because it means the Arm QEMU path
already has the exact shape wanted for an uncontended second serial channel.

### What This Means For The Plan

For QEMU-backed targets, the project should strongly prefer:

- a second dedicated QEMU-backed serial channel for `ConsoleMux`

over:

- continuing to frame mux traffic on the same serial/stdout path used for
  legacy console traffic

This suggests that the “harden noisy shared UART framing” work should be
treated as an interim coexistence measure, not as the target design for the
QEMU-backed development and validation path.

### Likely Near-Term Implementation Strategy

Without changing QEMU machine code first:

1. x86:
   - keep COM1 as legacy console
   - add a second `-serial` backend for COM2
   - bind `ConsoleMux` uplink to COM2

2. Arm `virt`:
   - keep UART0 as legacy/default console
   - add a second `-serial` backend so QEMU instantiates UART1
   - bind `ConsoleMux` uplink to UART1 at `0x09040000`

3. runner/runtime:
   - stop treating the mux uplink as merged stdout transport
   - connect router/demux tooling to the dedicated second channel instead

### QEMU Source Modification Outlook

Based on current evidence, `sources/qemu/` modifications do not appear to be
required for the first useful implementation on either QEMU-backed target.

QEMU source changes may still become desirable later for:

- cleaner launch ergonomics
- a more explicit named side-channel device
- tighter integration with the runner

But they do not appear to be necessary to prove and adopt the uncontended
uplink architecture.
- stream `1` contains only VM0 guest-console traffic
- stream `5` contains only VM1 guest-console traffic
- `driver-vm login:` appears on `driver_vm_console`
- at least one Arm app using the same shared seam gets generated stream
  registry support without app-specific stream-description files

## Risks And Open Questions

- It is not yet fully verified which existing producers bypass `Init` and would
  need their own mux inputs.
- The exact best physical-uart sink component still needs design work.
- Cross-arch reuse should not force x86 and Arm to share identical stream sets;
  the shared component should support a common contract with app-selected
  subsets.
- The shared component and templates must avoid importing x86-specific outer
  QEMU assumptions into the generic interface.
- The implicit-generation rule must be strong enough to cover standard apps
  automatically, but narrow enough to avoid treating every generic `PutChar`
  endpoint in the workspace as a semantic console stream.
- A sender-only stream model would misclassify guest-visible output generated
  inside VMM-side emulation modules such as PL011; the API must therefore
  support semantic override, not only sender-default emission.
- Additional stream ids are acceptable only as centrally generated declared
  extensions. If components start allocating arbitrary ad hoc lanes, the stream
  contract will become unstable across apps and harder for the router/runtime
  to validate.
- It is still an open design question whether the generated registry should be
  a standalone JSON artifact or folded into `console-manifest.json`; both are
  viable, but the latter may reduce moving parts.
- Automatic enumeration must remain opt-in. A broad “discover every UART-like
  endpoint” rule would likely overmatch and create unstable stream maps.
- The current x86 binary-frame path still has a separate live router/runtime
  bug where remote stream `1` and `5` frames exist but are not yet
  materialized locally in the corresponding guest-console logs.
- The migration should therefore be coexistence-first: keep the current x86
  path testable while the new mux seam is introduced.
