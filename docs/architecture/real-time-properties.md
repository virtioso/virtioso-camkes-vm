# Real-Time Properties and WCET Guarantees

This document explains the real-time properties of the TII seL4 virtualization platform, including Worst Case Execution Time (WCET) guarantees and their implications for safety-critical systems.

## Overview

seL4 is unique among hypervisors in providing **formally proven timing bounds**. This document explains what this means and how it applies to virtualized systems.

## What is WCET?

**Worst Case Execution Time (WCET)** is the maximum time an operation can take under any circumstances.

```
Execution Time Distribution:

    Frequency
       │
       │    ┌────┐
       │    │    │
       │   ┌┤    │
       │  ┌┤│    │
       │ ┌┤││    │
       │┌┤│││    │┐
       └┴┴┴┴┴────┴┴───────────────────────► Time
        Average        Worst Case
        Case           (WCET)

For safety-critical systems, the WORST CASE determines if deadlines can be met.
```

### Why Average Case is Insufficient

| System | Deadline | Average | Worst Case | Safe? |
|--------|----------|---------|------------|-------|
| Airbag | 10 ms | 1 ms | 5 ms | ✓ Yes |
| Airbag | 10 ms | 1 ms | 50 ms | ✗ No |
| ABS | 5 ms | 0.5 ms | 3 ms | ✓ Yes |
| ABS | 5 ms | 0.5 ms | 100 ms | ✗ No |

A system that "usually" meets its deadline is **not** acceptable for safety-critical applications.

## seL4 WCET Properties

### Kernel Design for Bounded Execution

seL4 achieves WCET guarantees through deliberate design:

```
┌─────────────────────────────────────────────────────────────────┐
│                    seL4 WCET Design                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  NO DYNAMIC MEMORY ALLOCATION                           │    │
│  │  - All kernel objects allocated at boot                 │    │
│  │  - No malloc/free during operation                      │    │
│  │  - No memory allocator latency spikes                   │    │
│  └─────────────────────────────────────────────────────────┘    │
│                              │                                   │
│                              ▼                                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  NO UNBOUNDED LOOPS                                     │    │
│  │  - All loops have proven iteration bounds               │    │
│  │  - No "while (condition)" without bound                 │    │
│  │  - Formally verified loop termination                   │    │
│  └─────────────────────────────────────────────────────────┘    │
│                              │                                   │
│                              ▼                                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  FULLY PREEMPTIBLE                                      │    │
│  │  - Kernel can be preempted almost anywhere              │    │
│  │  - Very short non-preemptible sections                  │    │
│  │  - No "big kernel lock"                                 │    │
│  └─────────────────────────────────────────────────────────┘    │
│                              │                                   │
│                              ▼                                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  O(1) SCHEDULING                                        │    │
│  │  - Constant-time scheduling decisions                   │    │
│  │  - No O(n) list traversals                              │    │
│  │  - Deterministic thread selection                       │    │
│  └─────────────────────────────────────────────────────────┘    │
│                              │                                   │
│                              ▼                                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  FORMAL VERIFICATION                                    │    │
│  │  - Mathematical proof of timing bounds                  │    │
│  │  - Machine-checked proofs (Isabelle/HOL)               │    │
│  │  - Covers all kernel paths                              │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Proven WCET Bounds

| Operation | WCET (cycles) | Notes |
|-----------|---------------|-------|
| **IPC (call)** | ~200 | Thread-to-thread message |
| **IPC (reply)** | ~200 | Response message |
| **Interrupt handling** | ~100 | To handler dispatch |
| **Context switch** | ~100-200 | Thread switch |
| **System call entry** | ~50 | Trap handling |
| **Total kernel path** | ~10,000 | Maximum for any syscall |

These bounds are **proven**, not measured. The formal verification guarantees no operation exceeds these bounds.

## seL4 MCS Kernel

The **Mixed Criticality Systems (MCS)** variant of seL4 provides additional real-time features:

### Scheduling Contexts

```
Standard seL4:                    seL4 MCS:
┌─────────────────────┐           ┌─────────────────────┐
│      Thread         │           │      Thread         │
│  ┌───────────────┐  │           │                     │
│  │ Priority      │  │           └──────────┬──────────┘
│  │ Time slice    │  │                      │
│  │ (global pool) │  │                      │ bound to
│  └───────────────┘  │                      ▼
└─────────────────────┘           ┌─────────────────────┐
                                  │ Scheduling Context  │
Thread owns its scheduling        │  ┌───────────────┐  │
                                  │  │ Budget: 10ms  │  │
                                  │  │ Period: 100ms │  │
                                  │  │ Priority: 200 │  │
                                  │  └───────────────┘  │
                                  └─────────────────────┘

                                  Scheduling separate from thread
```

### Budget Enforcement

```
CPU Time Allocation (MCS):
┌─────────────────────────────────────────────────────────────────┐
│ Time ──────────────────────────────────────────────────────────►│
├─────────────────────────────────────────────────────────────────┤
│ Period 1              │ Period 2              │ Period 3        │
│ ┌──────────┐          │ ┌──────────┐          │ ┌──────────┐    │
│ │ Budget A │          │ │ Budget A │          │ │ Budget A │    │
│ │  (10ms)  │          │ │  (10ms)  │          │ │  (10ms)  │    │
│ └──────────┴──────────┤ └──────────┴──────────┤ └──────────┘    │
│            │  Budget B│            │  Budget B│                 │
│            │  (5ms)   │            │  (5ms)   │                 │
│            └──────────┤            └──────────┤                 │
│                       │                       │                 │
│  100ms period         │  100ms period         │                 │
└─────────────────────────────────────────────────────────────────┘

Thread A: Guaranteed 10ms every 100ms period
Thread B: Guaranteed 5ms every 100ms period
CANNOT steal each other's time, even if malicious
```

### Passive Servers

MCS introduces "passive" servers that don't consume their own CPU budget:

```
Standard seL4:                    seL4 MCS (Passive Server):
┌──────────────┐                  ┌──────────────┐
│   Client     │                  │   Client     │
│  Budget: 50  │                  │  Budget: 50  │
└──────┬───────┘                  └──────┬───────┘
       │ IPC                             │ IPC
       ▼                                 ▼
┌──────────────┐                  ┌──────────────┐
│   Server     │                  │   Server     │
│  Budget: 50  │ ◄─ Uses own      │  (Passive)   │ ◄─ Uses CLIENT's
└──────────────┘    budget        └──────────────┘    budget

Server processing consumes         Server processing charged
server's time budget              to calling client
```

This enables:
- Accurate accounting of CPU usage
- Prevention of denial-of-service via server
- Fair sharing among clients

## Real-Time in Virtualized Systems

### What seL4 Guarantees for VMs

```
┌─────────────────────────────────────────────────────────────────┐
│                         seL4 Kernel                              │
│                      (WCET PROVEN)                               │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Guarantees:                                                     │
│  ✓ VM will be scheduled within bounded time                     │
│  ✓ VM exit handling has bounded latency                         │
│  ✓ Interrupt delivery to VM has bounded latency                 │
│  ✓ IPC between VMs (via VMM) has bounded latency               │
│                                                                  │
│  Does NOT guarantee:                                             │
│  ✗ What happens INSIDE the VM                                   │
│  ✗ Guest OS scheduling behavior                                 │
│  ✗ Guest application timing                                     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Guest OS Considerations

| Guest OS | Real-Time Capability | Notes |
|----------|---------------------|-------|
| Linux (standard) | Soft RT only | Unbounded latencies |
| Linux (PREEMPT_RT) | Improved soft RT | Still no guarantees |
| FreeRTOS | Hard RT | Designed for real-time |
| Zephyr | Hard RT | Certified for safety |
| seL4 native | Hard RT | No VM, direct on seL4 |

**For hard real-time within a VM:**
```
Option 1: Real-time guest OS
┌─────────────────────────────────────────┐
│  seL4 (WCET proven)                     │
│  └─► VM running FreeRTOS/Zephyr         │
│      └─► Real-time application          │
│          (Hard RT achievable)           │
└─────────────────────────────────────────┘

Option 2: Native seL4 thread (no VM)
┌─────────────────────────────────────────┐
│  seL4 (WCET proven)                     │
│  └─► Native seL4 thread                 │
│      └─► Real-time application          │
│          (Hard RT achievable)           │
└─────────────────────────────────────────┘

Option 3: Linux VM (NOT hard RT)
┌─────────────────────────────────────────┐
│  seL4 (WCET proven)                     │
│  └─► VM running Linux                   │
│      └─► Real-time application          │
│          (Soft RT only!)                │
└─────────────────────────────────────────┘
```

## The Secure World Caveat

**Critical limitation**: seL4's WCET guarantees only apply to the Normal World.

### ARM TrustZone Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                                                                  │
│   SECURE WORLD                    │    NORMAL WORLD              │
│   (TrustZone)                     │                              │
│                                   │                              │
│   ┌─────────────────────────┐     │    ┌─────────────────────┐  │
│   │ S-EL3: ARM Trusted      │     │    │ EL2: seL4           │  │
│   │        Firmware (ATF)   │     │    │      Hypervisor     │  │
│   ├─────────────────────────┤     │    ├─────────────────────┤  │
│   │ S-EL1: Secure OS        │     │    │ EL1: Guest VMs      │  │
│   │        (OP-TEE, etc.)   │     │    │      (Linux, etc.)  │  │
│   ├─────────────────────────┤     │    ├─────────────────────┤  │
│   │ S-EL0: Trusted Apps     │     │    │ EL0: Applications   │  │
│   └─────────────────────────┘     │    └─────────────────────┘  │
│                                   │                              │
│   ⚠️  NO WCET GUARANTEES          │    ✓ WCET PROVEN (seL4)     │
│                                   │                              │
└───────────────────────────────────┴──────────────────────────────┘
                    ▲
                    │
                SMC (Secure Monitor Call)
                BLOCKS Normal World until return
                LATENCY UNBOUNDED
```

### Impact on WCET

```
Scenario: seL4 running, SMC occurs

Time ────────────────────────────────────────────────────────────►

Normal World:
├───────────────────┤                    ├─────────────────────────
│  seL4 running     │                    │  seL4 resumes
│  (WCET holds)     │                    │  (WCET holds)
└───────────────────┴────────────────────┴─────────────────────────
                    │                    │
                    │ SMC call           │ SMC return
                    ▼                    ▲
Secure World:       ├────────────────────┤
                    │  Processing...     │
                    │  (NO TIME BOUND!)  │
                    └────────────────────┘

During SMC: seL4's WCET guarantee is SUSPENDED
After SMC return: WCET guarantee resumes
```

### Sources of SMC Calls

| Source | Trigger | Avoidable? |
|--------|---------|------------|
| PSCI | Power management | Configurable |
| SMCCC | ARM standard calls | Mostly |
| OP-TEE | Secure services | Yes |
| Firmware | Platform-specific | Varies |
| Implicit | Some CPU features | Platform-dependent |

### Mitigation Strategies

1. **Avoid SMC in critical paths**
   ```c
   // DON'T do this in real-time code:
   result = tee_invoke_command(...);  // SMC inside!

   // DO: Handle TEE operations in non-critical thread
   ```

2. **Audit Secure World code**
   - Review ATF for long-running operations
   - Measure Secure World latency
   - Remove unnecessary Secure World features

3. **Platform selection**
   - Choose platforms with minimal Secure World
   - Avoid platforms with mandatory Secure World calls
   - Consider platforms without TrustZone

4. **Configuration**
   - Disable PSCI if not needed
   - Minimize ATF services
   - Avoid OP-TEE in critical systems

## Practical Considerations

### Interrupt Latency

```
Interrupt → Application Response:

KVM/Linux path:
┌─────────┬──────────┬───────────┬──────────┬─────────┐
│Hardware │   KVM    │  Linux    │ Schedule │  App    │
│  IRQ    │  Exit    │  Handler  │  Switch  │ Handler │
└─────────┴──────────┴───────────┴──────────┴─────────┘
          └──────────────────────────────────────────┘
                    1-10 ms (UNBOUNDED)

seL4 path:
┌─────────┬──────────┬──────────┐
│Hardware │  seL4    │   App    │
│  IRQ    │  IPC     │ Handler  │
└─────────┴──────────┴──────────┘
          └──────────────────────┘
               <10 μs (BOUNDED)
```

### Scheduling Jitter

```
Thread scheduling precision:

Linux:
  ├── Target: every 10ms
  ├── Actual: 9.5ms, 10.2ms, 11.8ms, 9.1ms, 15.3ms, ...
  └── Jitter: up to several ms

seL4:
  ├── Target: every 10ms
  ├── Actual: 10.000ms, 10.001ms, 9.999ms, 10.002ms, ...
  └── Jitter: <10 μs
```

### Timer Resolution

| System | Timer Resolution | Notes |
|--------|------------------|-------|
| Linux (HZ=1000) | 1 ms | Standard kernel |
| Linux (HZ=100) | 10 ms | Some embedded |
| seL4 | Hardware-dependent | Often <1 μs |

## Safety Certification

### Supported Standards

| Standard | Level | Domain | seL4 Suitable? |
|----------|-------|--------|----------------|
| DO-178C | DAL-A | Avionics (catastrophic) | ✓ Yes |
| DO-178C | DAL-B | Avionics (hazardous) | ✓ Yes |
| ISO 26262 | ASIL-D | Automotive (highest) | ✓ Yes |
| ISO 26262 | ASIL-B | Automotive (medium) | ✓ Yes |
| IEC 62304 | Class C | Medical (highest) | ✓ Yes |
| IEC 61508 | SIL 4 | Industrial (highest) | ✓ Yes |
| EN 50128 | SIL 4 | Railway (highest) | ✓ Yes |

### Why seL4 Enables Certification

1. **Formal Verification**
   - Mathematical proof of functional correctness
   - Machine-checked (Isabelle/HOL)
   - Covers all kernel code paths

2. **WCET Proofs**
   - Timing bounds are proven, not estimated
   - No reliance on testing for timing

3. **Small TCB**
   - ~10K LOC is auditable
   - Complete code review feasible
   - All code paths analyzable

4. **No Undefined Behavior**
   - C code verified for absence of UB
   - No buffer overflows, null derefs, etc.

## Source Files

| File | Description |
|------|-------------|
| `kernel/` | seL4 kernel with timing properties |
| `configurations/` | Platform-specific timing configuration |

## Related Documentation

- [KVM/pKVM/seL4 Comparison](../appendix/kvm-pkvm-sel4-comparison.md) - Includes WCET comparison
- [System Overview](overview.md) - High-level architecture
- [seL4 Whitepaper](https://sel4.systems/About/seL4-whitepaper.pdf) - Official seL4 documentation
