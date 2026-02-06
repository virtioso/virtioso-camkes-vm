# Native seL4 GPU/Host1x Architecture

**Status**: Experimental Concept
**Target Hardware**: NVIDIA ARM64 SoCs (Tegra/Orin family)

## Executive Summary

This document proposes an alternative GPU virtualization architecture where a **native seL4 thread** manages the NVIDIA host1x engine and GPU directly, rather than assigning GPU ownership to a Linux VM. This enables capability-based multi-tenant GPU access with strong isolation guarantees.

## Problem Statement

### Current Approach: VM-Based GPU Ownership

In traditional virtualization (including our current seL4/CAmkES architecture), GPU access follows one of these patterns:

```
Pattern A: Dedicated GPU VM
┌─────────────────────────────────────────────────────────┐
│                        seL4                              │
│                                                          │
│  ┌─────────────────┐        ┌─────────────────┐         │
│  │    GPU VM       │        │   Workload VM   │         │
│  │                 │        │                 │         │
│  │  Linux + Mesa   │  RPC   │  Applications   │         │
│  │  host1x driver  │◄──────►│  (no GPU)       │         │
│  │  GPU driver     │        │                 │         │
│  │                 │        │                 │         │
│  │  [host1x + GPU] │        │  [no hardware]  │         │
│  └─────────────────┘        └─────────────────┘         │
│                                                          │
│  Problem: GPU VM has full Linux attack surface          │
│  Problem: Single point of GPU ownership                  │
└─────────────────────────────────────────────────────────┘

Pattern B: Passthrough to Application VM
┌─────────────────────────────────────────────────────────┐
│                        seL4                              │
│                                                          │
│  ┌─────────────────┐        ┌─────────────────┐         │
│  │   App VM (GUI)  │        │  App VM (LLM)   │         │
│  │                 │        │                 │         │
│  │  Linux + Mesa   │        │  Linux + CUDA   │         │
│  │  [host1x + GPU] │        │  [no GPU]       │         │
│  └─────────────────┘        └─────────────────┘         │
│                                                          │
│  Problem: Only ONE VM can use GPU                        │
│  Problem: Cannot share between GUI and compute           │
└─────────────────────────────────────────────────────────┘
```

### NVIDIA Host1x Challenge

On NVIDIA ARM64 SoCs (Tegra, Orin), the **host1x** engine is the central command processor:

```
NVIDIA Tegra/Orin Architecture:
┌─────────────────────────────────────────────────────────┐
│                     Host1x Engine                        │
│              (Command FIFO, Sync Points)                 │
│                          │                               │
│    ┌─────────┬──────────┼──────────┬─────────┐          │
│    │         │          │          │         │          │
│    ▼         ▼          ▼          ▼         ▼          │
│ ┌─────┐  ┌─────┐   ┌─────────┐  ┌─────┐  ┌─────┐       │
│ │ GPU │  │ VIC │   │ Display │  │NVDEC│  │NVENC│       │
│ │     │  │     │   │         │  │     │  │     │       │
│ └─────┘  └─────┘   └─────────┘  └─────┘  └─────┘       │
│                                                          │
│  Host1x manages ALL engines - cannot split ownership     │
└─────────────────────────────────────────────────────────┘
```

**Key insight**: Host1x is tightly coupled to all NVIDIA engines. In Linux, the `tegra-host1x` driver must coordinate all clients. Splitting GPU ownership between VMs is problematic because host1x cannot be partitioned.

## Proposed Architecture: Native seL4 GPU Manager

### Core Concept

Instead of giving a Linux VM ownership of host1x/GPU, implement a **native seL4 component** that:

1. Directly manages host1x hardware (no Linux, no VM overhead)
2. Exposes GPU capabilities via seL4 IPC endpoints
3. Uses seL4 capabilities for fine-grained access control
4. Enables multiple VMs to share GPU resources safely

```
┌─────────────────────────────────────────────────────────────────────┐
│                              seL4                                    │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │              Native Host1x Manager (seL4 Thread)              │   │
│  │                                                                │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐           │   │
│  │  │ GPU Context │  │ GPU Context │  │ GPU Context │           │   │
│  │  │   (VM1)     │  │   (VM2)     │  │   (VM3)     │           │   │
│  │  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘           │   │
│  │         │                │                │                   │   │
│  │         ▼                ▼                ▼                   │   │
│  │  ┌────────────────────────────────────────────────────────┐  │   │
│  │  │              Host1x Command Scheduler                   │  │   │
│  │  │         (Priority, Quotas, Preemption)                  │  │   │
│  │  └────────────────────────────────────────────────────────┘  │   │
│  │                            │                                  │   │
│  │                            ▼                                  │   │
│  │                     [host1x + GPU]                           │   │
│  └──────────────────────────────────────────────────────────────┘   │
│         ▲                    ▲                    ▲                  │
│         │ IPC                │ IPC                │ IPC              │
│         │ (Cap)              │ (Cap)              │ (Cap)            │
│  ┌──────┴──────┐      ┌──────┴──────┐      ┌──────┴──────┐          │
│  │   GUI VM    │      │   LLM VM    │      │  Video VM   │          │
│  │             │      │             │      │             │          │
│  │ Wayland/X11 │      │  CUDA/LLM   │      │  Encode/    │          │
│  │  Rendering  │      │  Inference  │      │  Decode     │          │
│  │             │      │             │      │             │          │
│  │ [no GPU HW] │      │ [no GPU HW] │      │ [no GPU HW] │          │
│  └─────────────┘      └─────────────┘      └─────────────┘          │
│                                                                      │
│  Each VM has GPU capability, but no direct hardware access           │
└─────────────────────────────────────────────────────────────────────┘
```

### Key Benefits

| Aspect | VM-Based GPU | Native seL4 GPU Manager |
|--------|--------------|-------------------------|
| **TCB for GPU** | Full Linux kernel | Small seL4 thread |
| **Multi-VM GPU** | Impossible or requires para-virt | Native support |
| **Access Control** | None (VM owns GPU) | Capability-based |
| **Scheduling** | Linux scheduler | Custom, priority-aware |
| **Isolation** | VM boundary only | Per-context isolation |
| **Preemption** | GPU-level only | seL4-managed |
| **Resource Quotas** | Difficult | Native enforcement |

### Capability-Based Access Control

seL4 capabilities enable fine-grained GPU access control:

```
┌─────────────────────────────────────────────────────────┐
│              Capability Distribution                     │
│                                                          │
│  GPU Manager holds:                                      │
│    - Device capability (host1x, GPU MMIO)               │
│    - IRQ capability (host1x, GPU interrupts)            │
│    - Memory capabilities (VRAM, command buffers)        │
│                                                          │
│  GUI VM receives:                                        │
│    - RenderContext capability                           │
│    - Framebuffer memory capability                      │
│    - Display output capability                          │
│                                                          │
│  LLM VM receives:                                        │
│    - ComputeContext capability                          │
│    - Tensor memory capability (large allocation)        │
│    - NO display capability (cannot access framebuffer)  │
│                                                          │
│  Video VM receives:                                      │
│    - NVDEC context capability                           │
│    - NVENC context capability                           │
│    - DMA buffer capabilities                            │
│    - NO compute capability (cannot run CUDA)            │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### IPC Interface Design

VMs communicate with GPU Manager via seL4 IPC:

```c
/* GPU Manager IPC Interface (Conceptual) */

/* Create execution context for caller VM */
seL4_MessageInfo_t gpu_create_context(
    seL4_CPtr gpu_endpoint,      /* IPC endpoint to GPU Manager */
    gpu_context_type_t type,     /* RENDER, COMPUTE, VIDEO_DECODE, etc. */
    size_t memory_quota,         /* Maximum VRAM allocation */
    uint32_t priority            /* Scheduling priority */
);
/* Returns: Context capability */

/* Submit command buffer */
seL4_MessageInfo_t gpu_submit_commands(
    seL4_CPtr context,           /* Context capability */
    seL4_CPtr cmd_buffer,        /* Shared memory with commands */
    size_t cmd_size,             /* Command buffer size */
    seL4_CPtr fence_ntfn         /* Notification for completion */
);
/* Returns: Fence ID for synchronization */

/* Allocate GPU memory */
seL4_MessageInfo_t gpu_alloc_memory(
    seL4_CPtr context,           /* Context capability */
    size_t size,                 /* Requested size */
    gpu_memory_flags_t flags     /* VRAM, system, cached, etc. */
);
/* Returns: Memory capability, GPU virtual address */

/* Wait for fence */
seL4_MessageInfo_t gpu_wait_fence(
    seL4_CPtr context,           /* Context capability */
    uint64_t fence_id            /* Fence from submit_commands */
);
```

### Command Buffer Model

VMs prepare GPU commands in shared memory, GPU Manager validates and submits:

```
┌─────────────────────────────────────────────────────────┐
│                  Command Flow                            │
│                                                          │
│  1. VM allocates command buffer (shared memory)         │
│  2. VM writes GPU commands (validated format)           │
│  3. VM calls gpu_submit_commands() via IPC              │
│  4. GPU Manager validates commands:                     │
│     - Memory references within VM's allocation          │
│     - No privileged operations                          │
│     - Resource usage within quota                       │
│  5. GPU Manager submits to host1x                       │
│  6. Host1x executes on GPU                              │
│  7. Completion triggers fence notification              │
│  8. VM receives notification, can use results           │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

## Implementation Considerations

### Host1x Driver in seL4

Would need to implement host1x driver as native seL4 code:

```
┌─────────────────────────────────────────────────────────┐
│           Host1x Driver Components                       │
│                                                          │
│  ┌────────────────────────────────────────────────────┐ │
│  │              Command FIFO Manager                   │ │
│  │  - Per-context command queues                      │ │
│  │  - Priority-based scheduling                       │ │
│  │  - Preemption support                              │ │
│  └────────────────────────────────────────────────────┘ │
│                                                          │
│  ┌────────────────────────────────────────────────────┐ │
│  │              Sync Point Manager                     │ │
│  │  - Fence creation and signaling                    │ │
│  │  - Cross-context synchronization                   │ │
│  │  - Timeout handling                                │ │
│  └────────────────────────────────────────────────────┘ │
│                                                          │
│  ┌────────────────────────────────────────────────────┐ │
│  │              Memory Manager                         │ │
│  │  - IOMMU/SMMU configuration                        │ │
│  │  - Per-context address spaces                      │ │
│  │  - VRAM allocation and tracking                    │ │
│  └────────────────────────────────────────────────────┘ │
│                                                          │
│  ┌────────────────────────────────────────────────────┐ │
│  │              GPU Client Interfaces                  │ │
│  │  - 3D graphics (for Mesa/OpenGL)                   │ │
│  │  - Compute (for CUDA-like workloads)               │ │
│  │  - Video encode/decode (NVDEC/NVENC)               │ │
│  └────────────────────────────────────────────────────┘ │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### Guest Mesa/Driver Integration

Guest VMs would need modified graphics stack:

```
Guest VM Graphics Stack:
┌─────────────────────────────────────────────────────────┐
│                    Application                           │
│                        │                                 │
│                        ▼                                 │
│  ┌────────────────────────────────────────────────────┐ │
│  │                Mesa/Gallium                         │ │
│  │          (Standard OpenGL/Vulkan API)              │ │
│  └────────────────────────────────────────────────────┘ │
│                        │                                 │
│                        ▼                                 │
│  ┌────────────────────────────────────────────────────┐ │
│  │         seL4 GPU Driver (DRM-like)                 │ │
│  │  - Translates to GPU Manager IPC                   │ │
│  │  - Manages local command buffers                   │ │
│  │  - Handles fence synchronization                   │ │
│  └────────────────────────────────────────────────────┘ │
│                        │                                 │
│                        │ seL4 IPC                        │
│                        ▼                                 │
│              [To Native GPU Manager]                     │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### Challenges and Mitigations

| Challenge | Mitigation |
|-----------|------------|
| **Complexity of host1x** | Start with subset (compute only), expand incrementally |
| **GPU driver size** | Focus on specific use cases, not full Mesa compatibility |
| **Performance overhead** | Zero-copy command buffers, batched submissions |
| **Firmware blobs** | May require secure world loading (TrustZone) |
| **Nouveau vs NVIDIA driver** | Evaluate both, nouveau more open but less features |
| **GPU context switching** | Leverage host1x hardware scheduling |

### Comparison with Alternatives

| Approach | Isolation | Multi-VM | Complexity | Performance |
|----------|-----------|----------|------------|-------------|
| **Full passthrough** | VM-level | No | Low | Best |
| **SR-IOV (if available)** | Hardware | Yes | Medium | Good |
| **Virtio-gpu** | Emulation | Yes | High | Poor |
| **Native seL4 GPU** | Capability | Yes | High (initial) | Good |

### Incremental Development Path

```
Phase 1: Proof of Concept
├── Basic host1x register access from seL4
├── Single compute context
├── Fixed memory allocation
└── Simple IPC interface

Phase 2: Multi-Context
├── Multiple contexts (one per VM)
├── Context scheduling
├── Dynamic memory allocation
└── Fence synchronization

Phase 3: Full Features
├── 3D graphics support
├── Video encode/decode
├── Display output
├── Advanced scheduling (quotas, priorities)

Phase 4: Production
├── Security hardening
├── Performance optimization
├── Mesa/Vulkan integration
└── CUDA subset support
```

## Use Cases

### Use Case 1: Secure GUI + LLM Inference

```
┌─────────────────────────────────────────────────────────┐
│                     Deployment                           │
│                                                          │
│  GUI VM (isolated):                                      │
│    - Wayland compositor                                  │
│    - User applications                                   │
│    - Uses GPU for rendering                              │
│    - 30% GPU quota                                       │
│                                                          │
│  LLM VM (isolated):                                      │
│    - LLaMA/similar inference                             │
│    - Uses GPU for matrix operations                      │
│    - 70% GPU quota                                       │
│    - Cannot access GUI framebuffer                       │
│                                                          │
│  Both share physical GPU via Native GPU Manager          │
│  seL4 guarantees neither can access other's GPU memory   │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### Use Case 2: Video Conferencing with Privacy

```
┌─────────────────────────────────────────────────────────┐
│                     Deployment                           │
│                                                          │
│  Camera VM:                                              │
│    - Camera capture                                      │
│    - NVENC for compression                               │
│    - Cannot access other VMs' video                      │
│                                                          │
│  Conference VM:                                          │
│    - NVDEC for remote streams                            │
│    - Display compositing                                 │
│    - Network interface                                   │
│                                                          │
│  Local Processing VM:                                    │
│    - Background blur (GPU compute)                       │
│    - Face tracking                                       │
│    - Isolated from network                               │
│                                                          │
│  Each VM has minimal GPU capabilities needed             │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

## Relationship to Current Architecture

This proposal extends the existing Virtioso seL4 virtio architecture:

- **Complements virtio**: GPU Manager is another "device" like virtio-net, virtio-blk
- **Uses same IPC patterns**: seL4 IPC, CAmkES connections, shared memory
- **Fits topology model**: GPU Manager is a peer, not a special host
- **Enables new scenarios**: Multi-tenant GPU previously impossible

```
┌─────────────────────────────────────────────────────────────────────┐
│                              seL4                                    │
│                                                                      │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐      │
│  │  Native GPU     │  │ Device VM       │  │  Driver VM      │      │
│  │  Manager        │  │ (virtio-net,    │  │  (Applications) │      │
│  │                 │  │  virtio-blk)    │  │                 │      │
│  │  [host1x+GPU]   │  │  [NIC + eMMC]   │  │  [no hardware]  │      │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘      │
│           │                    │                    │                │
│           │◄───────────────────┼────────────────────┤                │
│           │  GPU IPC           │  virtio RPC        │                │
│           │                    │                    │                │
│           ▼                    ▼                    ▼                │
│                                                                      │
│  Native seL4 threads and Linux VMs coexist as peers                  │
└─────────────────────────────────────────────────────────────────────┘
```

## Conclusion

A native seL4 GPU Manager offers unique advantages for multi-tenant GPU scenarios on NVIDIA ARM64 platforms. By removing Linux from the GPU management path and using seL4 capabilities for access control, we can achieve:

- **Strong isolation**: Capability-based, formally verifiable
- **Multi-VM GPU sharing**: Not possible with passthrough
- **Minimal TCB**: No Linux kernel in GPU path
- **Flexible scheduling**: Priority-based, quota-enforced

This approach aligns with seL4's philosophy of minimal trusted computing base and capability-based security, while addressing a real limitation in current GPU virtualization approaches.

## Related Documentation

- [VM Topology](../../architecture/vm-topology.md) - Flexible N:M VM relationships
- [KVM/pKVM/seL4 Comparison](../../appendix/kvm-pkvm-sel4-comparison.md) - Architectural differences
- [Memory Model](../../architecture/memory-model.md) - Shared memory patterns

## References

- NVIDIA Tegra host1x documentation
- Nouveau driver source (open-source reference)
- seL4 IPC and capability system
- Mesa Gallium driver architecture
