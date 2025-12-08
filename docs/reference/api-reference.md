# API Reference

This document provides a comprehensive reference for data structures and APIs used in the TII seL4 virtio system.

## Core Data Structures

### rpcmsg_t

RPC message structure for VMM ↔ QEMU communication.

```c
// include/sel4/rpc_queue.h
typedef struct rpcmsg {
    seL4_Word mr0;    // Opcode and flags
    seL4_Word mr1;    // Parameter 1 (typically address)
    seL4_Word mr2;    // Parameter 2 (typically data/size)
    seL4_Word mr3;    // Parameter 3 (additional info)
} rpcmsg_t;
```

**Fields:**
| Field | Type | Description |
|-------|------|-------------|
| `mr0` | `seL4_Word` | Operation code and encoded parameters |
| `mr1` | `seL4_Word` | Context-dependent (address, IRQ number) |
| `mr2` | `seL4_Word` | Context-dependent (data, level) |
| `mr3` | `seL4_Word` | Context-dependent (additional data) |

### vso_rpc_t

Main RPC context structure.

```c
// include/sel4/rpc.h
typedef struct vso_rpc {
    vso_driver_rpc_t driver_rpc;      // Driver-to-device RPC
    vso_device_event_t device_event;   // Device events

    void (*doorbell)(void *cookie);    // Doorbell callback
    void *doorbell_cookie;             // Callback cookie
} vso_rpc_t;
```

**Fields:**
| Field | Type | Description |
|-------|------|-------------|
| `driver_rpc` | `vso_driver_rpc_t` | RPC request/response queues |
| `device_event` | `vso_device_event_t` | Event queue for device → driver |
| `doorbell` | function pointer | Called to signal new messages |
| `doorbell_cookie` | `void *` | Passed to doorbell callback |

### vso_driver_rpc_t

Driver RPC queue structure.

```c
// include/sel4/rpc.h
typedef struct vso_driver_rpc {
    rpcmsg_rpc_queue_t request;        // Request queue
    rpcmsg_rpc_queue_t response;       // Response queue
    rpcmsg_buffer_state_t buffer_state; // Buffer management
} vso_driver_rpc_t;
```

### io_proxy_t

I/O proxy structure for managing VM-to-QEMU communication.

```c
// include/tii/io_proxy.h
typedef struct io_proxy {
    vm_t *vm;                          // Associated VM
    vso_rpc_t rpc;                     // RPC context
    void *iobuf;                       // I/O buffer mapping
    void *memdev;                      // Memory device mapping
    size_t memdev_size;                // Memory device size

    // Callback handlers
    io_proxy_callback_t *callbacks;    // Callback chain
    void *callback_cookie;             // Callback data
} io_proxy_t;
```

**Fields:**
| Field | Type | Description |
|-------|------|-------------|
| `vm` | `vm_t *` | Target VM for this proxy |
| `rpc` | `vso_rpc_t` | RPC communication context |
| `iobuf` | `void *` | Shared I/O buffer |
| `memdev` | `void *` | Shared memory device |
| `memdev_size` | `size_t` | Size of memory device |
| `callbacks` | `io_proxy_callback_t *` | Request handlers |
| `callback_cookie` | `void *` | User data for callbacks |

### irq_line_t

Single IRQ line structure.

```c
// include/tii/irq_line.h
typedef struct irq_line {
    vm_t *vm;           // Target VM
    int irq;            // IRQ number
    bool level;         // Current level (for level-triggered)
} irq_line_t;
```

### shared_irq_line_t

Shared IRQ line for multiple sources.

```c
// include/tii/shared_irq_line.h
typedef struct shared_irq_line {
    uint64_t sources;   // Bitmap of active sources
    int irq;            // Guest IRQ number
    vm_t *vm;           // Target VM
} shared_irq_line_t;
```

### gicv2m_t

GICv2m MSI controller structure.

```c
// include/tii/gicv2m.h
typedef struct gicv2m {
    vm_t *vm;           // Associated VM
    uintptr_t base;     // GICv2m frame base address
    int spi_base;       // First SPI number
    int spi_count;      // Number of available SPIs
} gicv2m_t;
```

## RPC Queue Data Structures

These structures from `rpc_queue.h` implement the lock-free message queues used for VMM ↔ QEMU communication.

### rpcmsg_marker_t

Atomic marker for lock-free queue operations.

```c
// include/sel4/rpc_queue.h
typedef union rpcmsg_marker {
    _Atomic uint64_t pair;       // Atomic access to both fields
    struct {
        uint32_t pos;            // Position in ring buffer (0 to RPCMSG_BUFFER_SIZE-1)
        uint32_t count;          // Monotonic sequence counter (prevents ABA problem)
    };
} rpcmsg_marker_t;
```

**Fields:**
| Field | Type | Description |
|-------|------|-------------|
| `pair` | `_Atomic uint64_t` | Combined atomic access to both pos and count |
| `pos` | `uint32_t` | Current position in ring buffer (0-31) |
| `count` | `uint32_t` | Monotonically increasing counter |

**Purpose:** The combined `pos + count` prevents the ABA problem in lock-free operations. The `count` monotonically increases with each operation, ensuring that even if `pos` wraps around to the same value, the overall marker value differs.

### rpcmsg_queue_bound_t

Producer/consumer bounds containing head and tail markers.

```c
// include/sel4/rpc_queue.h
typedef struct rpcmsg_queue_bound {
    volatile rpcmsg_marker_t tail;  // Claimed position (in-progress)
    volatile rpcmsg_marker_t head;  // Committed position (visible)
} rpcmsg_queue_bound_t;
```

**Fields:**
| Field | Type | Description |
|-------|------|-------------|
| `tail` | `rpcmsg_marker_t` | Position being claimed (enqueue/dequeue in progress) |
| `head` | `rpcmsg_marker_t` | Committed position (visible to other side) |

### rpcmsg_queue_t

Queue metadata with producer/consumer bounds and ring buffer.

```c
// include/sel4/rpc_queue.h
typedef struct rpcmsg_queue_t {
    volatile rpcmsg_queue_bound_t prod;  // Producer bounds
    volatile rpcmsg_queue_bound_t cons;  // Consumer bounds
    uint16_t ring[RPCMSG_BUFFER_SIZE];   // Ring of message IDs
} rpcmsg_queue_t;
```

**Fields:**
| Field | Type | Description |
|-------|------|-------------|
| `prod` | `rpcmsg_queue_bound_t` | Producer head/tail (enqueue operations) |
| `cons` | `rpcmsg_queue_bound_t` | Consumer head/tail (dequeue operations) |
| `ring` | `uint16_t[32]` | Ring buffer containing message buffer indices |

**Lock-Free Design:**
The split prod/cons bounds enable MPMC (multi-producer, multi-consumer) lock-free operations:
- Producer: atomically claims `prod.tail`, writes to `ring[]`, updates `prod.head`
- Consumer: atomically claims `cons.tail`, reads from `ring[]`, updates `cons.head`

### rpcmsg_buffer_t

Message storage ring buffer.

```c
// include/sel4/rpc_queue.h
#define RPCMSG_BUFFER_SIZE 32

typedef struct rpcmsg_buffer {
    rpcmsg_t messages[RPCMSG_BUFFER_SIZE];  // 32 messages × 32 bytes = 1024 bytes
} rpcmsg_buffer_t;
```

**Fields:**
| Field | Type | Description |
|-------|------|-------------|
| `messages` | `rpcmsg_t[32]` | Ring buffer of RPC messages |

### rpcmsg_iobuf_t

Complete shared memory layout for RPC communication.

```c
// include/sel4/rpc_queue.h
typedef struct rpcmsg_iobuf {
    rpcmsg_buffer_t buffers[2];  // Message storage
    rpcmsg_queue_t queues[4];    // Queue metadata
} rpcmsg_iobuf_t;
```

**Buffer IDs:**
| ID | Constant | Purpose |
|----|----------|---------|
| 0 | `rpcmsg_buffer_id_drvrpc` | Driver RPC messages (MMIO req/resp) |
| 1 | `rpcmsg_buffer_id_devevt` | Device event messages (IRQ, PCI) |

**Queue IDs:**
| ID | Constant | Direction | Purpose |
|----|----------|-----------|---------|
| 0 | `queue_id_drvrpc_req` | VMM → Device (kernel) | MMIO requests |
| 1 | `queue_id_drvrpc_req_dev` | Kernel → Userspace | Request forwarding |
| 2 | `queue_id_drvrpc_resp` | Device → VMM | MMIO responses |
| 3 | `queue_id_devevt` | Device → VMM | Async events |

### rpcmsg_rpc_queue_t

Combined queue reference with buffer pointer.

```c
// include/sel4/rpc_queue.h
typedef struct rpcmsg_rpc_queue {
    rpcmsg_queue_t *queue;       // Queue metadata pointer
    rpcmsg_buffer_t *buffer;     // Associated message buffer
} rpcmsg_rpc_queue_t;
```

**Fields:**
| Field | Type | Description |
|-------|------|-------------|
| `queue` | `rpcmsg_queue_t *` | Pointer to queue metadata |
| `buffer` | `rpcmsg_buffer_t *` | Pointer to message storage |

### Queue Access Macros

The `iobuf_queue` macro creates an `rpcmsg_rpc_queue_t` or `rpcmsg_event_queue_t` struct by pairing a buffer and queue from the iobuf:

```c
// Core macro - creates queue struct from iobuf pointer
#define iobuf_queue(_addr, _bid, _qid, _type) ({      \
    rpcmsg_iobuf_t *_iobuf = (void *)(_addr);         \
    _type _q = {                                       \
        .buffer = &_iobuf->buffers[(_bid)],           \
        .queue = &_iobuf->queues[(_qid)],             \
    };                                                 \
    _q;                                                \
})

// Driver-side (VMM) queue access - returns rpcmsg_rpc_queue_t
#define driver_drvrpc_req(iobuf) \
    iobuf_queue((iobuf), iobuf_id_drvrpc, queue_id_drvrpc_req, rpcmsg_rpc_queue_t)

#define driver_drvrpc_resp(iobuf) \
    iobuf_queue((iobuf), iobuf_id_drvrpc, queue_id_drvrpc_resp, rpcmsg_rpc_queue_t)

// Device kernel module queue access
#define device_km_drvrpc_req(iobuf) \
    iobuf_queue((iobuf), iobuf_id_drvrpc, queue_id_drvrpc_req, rpcmsg_rpc_queue_t)

#define device_km_drvrpc_resp(iobuf) \
    iobuf_queue((iobuf), iobuf_id_drvrpc, queue_id_drvrpc_resp, rpcmsg_rpc_queue_t)

// Device userspace (QEMU) queue access
#define device_drvrpc_req(iobuf) \
    iobuf_queue((iobuf), iobuf_id_drvrpc, queue_id_drvrpc_req_dev, rpcmsg_rpc_queue_t)

#define device_drvrpc_resp(iobuf) \
    iobuf_queue((iobuf), iobuf_id_drvrpc, queue_id_drvrpc_resp, rpcmsg_rpc_queue_t)

// Event queue - returns rpcmsg_event_queue_t
#define devevt_queue(iobuf) \
    iobuf_queue((iobuf), iobuf_id_devevt, queue_id_devevt, rpcmsg_event_queue_t)
```

**Note:** These macros return struct values (not pointers). They extract both the buffer and queue from the iobuf and combine them into a typed queue handle.

### Lock-Free Queue Semantics

The queue implementation provides lock-free MPMC (multi-producer, multi-consumer) semantics using a two-phase commit approach:

**Empty Check:**
```c
// Queue is empty when producer committed head equals consumer committed head
static inline bool rpcmsg_queue_empty(rpcmsg_queue_t *q) {
    return q->prod.head.val == q->cons.head.val;
}
```

**Full Check:**
```c
// Queue is full when all buffer slots are claimed
static inline bool rpcmsg_queue_full(rpcmsg_queue_t *q) {
    return (RPCMSG_BUFFER_SIZE + q->cons.tail.val - q->prod.tail.val) == 0;
}
```

**Enqueue Operation (Two-Phase):**
1. **Claim phase** (`rpcmsg_acquire_prod_entry`):
   - Atomically read `prod.tail`
   - Wait if head-tail distance exceeds threshold
   - Check available space against `cons.head`
   - CAS on `prod.tail` to claim slot
2. **Commit phase** (`rpcmsg_commit_update`):
   - Write message ID to `ring[pos]`
   - Atomically update `prod.head` to make visible

**Dequeue Operation (Two-Phase):**
1. **Claim phase** (`rpcmsg_acquire_cons_entry`):
   - Atomically read `cons.tail`
   - Check available entries against `prod.head`
   - CAS on `cons.tail` to claim slot
2. **Commit phase** (`rpcmsg_commit_update`):
   - Read message ID from `ring[pos]`
   - Atomically update `cons.head` to release

**Key Design Properties:**
- **ABA Prevention**: The `count` field in `rpcmsg_marker_t` monotonically increases
- **Head-Tail Distance**: Limits in-flight operations via `RPCMSG_HTD_MAX`
- **Two-Phase Commit**: Separates claiming from visibility for concurrent access

## I/O Proxy API

### io_proxy_init()

Initialize an I/O proxy instance.

```c
int io_proxy_init(
    io_proxy_t *io_proxy,
    vm_t *vm,
    void *iobuf,
    size_t iobuf_size,
    void *memdev,
    size_t memdev_size,
    void (*notify)(void *),
    void (*downcall)(void *)
);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `io_proxy` | `io_proxy_t *` | Proxy to initialize |
| `vm` | `vm_t *` | Associated VM |
| `iobuf` | `void *` | I/O buffer address |
| `iobuf_size` | `size_t` | I/O buffer size |
| `memdev` | `void *` | Memory device address |
| `memdev_size` | `size_t` | Memory device size |
| `notify` | function | Notification callback |
| `downcall` | function | Downcall callback |

**Returns:** 0 on success, negative error code on failure.

### io_proxy_handle_event()

Process pending RPC events.

```c
int io_proxy_handle_event(io_proxy_t *io_proxy);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `io_proxy` | `io_proxy_t *` | Proxy to process |

**Returns:** Number of events processed.

### ioreq_native()

Send a native MMIO request.

```c
seL4_Word ioreq_native(
    io_proxy_t *io_proxy,
    int addr_space,
    uint64_t addr,
    size_t size,
    bool is_write,
    seL4_Word data
);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `io_proxy` | `io_proxy_t *` | I/O proxy |
| `addr_space` | `int` | Address space (AS_GLOBAL or AS_PCIDEV) |
| `addr` | `uint64_t` | MMIO address |
| `size` | `size_t` | Access size (1, 2, 4, 8) |
| `is_write` | `bool` | true for write, false for read |
| `data` | `seL4_Word` | Data for write operations |

**Returns:** Read data or 0 for writes.

## vso_rpc API

### vso_rpc_init()

Initialize RPC context.

```c
int vso_rpc_init(
    vso_rpc_t *rpc,
    vso_rpc_id_t id,
    void *iobuf,
    void (*doorbell)(void *),
    void *doorbell_cookie
);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `rpc` | `vso_rpc_t *` | RPC context to initialize |
| `id` | `vso_rpc_id_t` | Role (driver, device_km, device) |
| `iobuf` | `void *` | Shared I/O buffer |
| `doorbell` | function | Doorbell callback |
| `doorbell_cookie` | `void *` | Callback cookie |

### vso_doorbell()

Ring the doorbell to signal new messages.

```c
int vso_doorbell(vso_rpc_t *rpc);
```

### driver_rpc_request()

Send an RPC request.

```c
int driver_rpc_request(
    vso_rpc_t *rpc,
    unsigned int op,
    seL4_Word mr0,
    seL4_Word mr1,
    seL4_Word mr2,
    seL4_Word mr3
);
```

### driver_rpc_reply()

Send an RPC reply.

```c
int driver_rpc_reply(vso_rpc_t *rpc, rpcmsg_t *msg);
```

### device_event_tx()

Send a device event.

```c
int device_event_tx(
    vso_rpc_t *rpc,
    unsigned int op,
    seL4_Word mr0,
    seL4_Word mr1,
    seL4_Word mr2,
    seL4_Word mr3
);
```

## IRQ API

### irq_line_init()

Initialize a single IRQ line.

```c
int irq_line_init(irq_line_t *line, vm_t *vm, int irq);
```

### irq_line_set()

Set IRQ level (for level-triggered interrupts).

```c
void irq_line_set(irq_line_t *line, int level);
```

### irq_line_pulse()

Pulse IRQ (for edge-triggered interrupts).

```c
void irq_line_pulse(irq_line_t *line);
```

### shared_irq_line_init()

Initialize a shared IRQ line.

```c
int shared_irq_line_init(shared_irq_line_t *line, vm_t *vm, int irq);
```

### shared_irq_set()

Set a source active on shared line.

```c
void shared_irq_set(shared_irq_line_t *line, int source);
```

### shared_irq_clear()

Clear a source on shared line.

```c
void shared_irq_clear(shared_irq_line_t *line, int source);
```

### shared_irq_pulse()

Pulse a source on shared line.

```c
void shared_irq_pulse(shared_irq_line_t *line, int source);
```

## GICv2m MSI API

### gicv2m_t

GICv2m MSI controller structure (full definition).

```c
// include/tii/gicv2m.h
#define GICV2M_IRQ_MAX 128

typedef struct gicv2m {
    uintptr_t base;                    // GICv2m frame base address
    size_t size;                       // Frame size (typically 4KB)
    irq_line_t irq[GICV2M_IRQ_MAX];   // Pre-initialized IRQ lines
    uint32_t irq_base;                 // First SPI number
    uint32_t num_irq;                  // Number of MSI-capable SPIs
} gicv2m_t;
```

### v2m_init()

Initialize GICv2m emulation and register memory fault handler.

```c
int v2m_init(gicv2m_t *s, vm_t *vm);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `s` | `gicv2m_t *` | Pre-configured GICv2m structure |
| `vm` | `vm_t *` | Target VM |

**Returns:** 0 on success, -1 on error.

**Notes:**
- The `gicv2m_t` structure must be pre-configured with `base`, `size`, `irq_base`, and `num_irq`
- Initializes all IRQ lines in the `irq[]` array
- Reserves memory region at `base` for fault handling

### v2m_irq_valid()

Check if an IRQ number is within the MSI range.

```c
bool v2m_irq_valid(gicv2m_t *s, uint32_t irq);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `s` | `gicv2m_t *` | GICv2m instance |
| `irq` | `uint32_t` | IRQ number to check |

**Returns:** `true` if IRQ is in range `[irq_base, irq_base + num_irq)`.

### v2m_inject_irq()

Inject an MSI interrupt to the guest.

```c
int v2m_inject_irq(gicv2m_t *s, uint32_t irq);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `s` | `gicv2m_t *` | GICv2m instance |
| `irq` | `uint32_t` | SPI number to inject |

**Returns:** 0 on success, -1 if IRQ out of range.

**Notes:**
- Internally calls `irq_line_pulse()` for edge-triggered behavior
- IRQ must be in valid MSI range

### msi_init()

Platform-specific MSI initialization (weak symbol).

```c
int msi_init(vm_t *vm);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `vm` | `vm_t *` | Target VM |

**Returns:** 0 on success, negative on error.

**Notes:**
- Default implementation is a no-op stub
- Platform-specific versions (e.g., `src/plat/rpi4/msi.c`) override this

### handle_msi()

RPC callback for MSI interrupt handling (weak symbol).

```c
int handle_msi(io_proxy_t *io_proxy, unsigned int op, rpcmsg_t *msg);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `io_proxy` | `io_proxy_t *` | I/O proxy context |
| `op` | `unsigned int` | RPC operation code |
| `msg` | `rpcmsg_t *` | RPC message |

**Returns:**
| Value | Meaning |
|-------|---------|
| `RPCMSG_RC_NONE` | Not an MSI IRQ, try other handlers |
| `RPCMSG_RC_HANDLED` | MSI processed successfully |
| `RPCMSG_RC_ERROR` | Error during MSI injection |

**Notes:**
- Called from IRQ handling chain for `QEMU_OP_SET_IRQ`
- Checks if `msg->mr1` (IRQ number) is in MSI range
- Platform-specific implementations handle actual injection

## FDT API

### DEFINE_FDT_NODE()

Register an FDT node generator.

```c
// include/tii/fdt.h
#define DEFINE_FDT_NODE(name, fn) \
    static fdt_node_t __fdt_node_##name \
    __attribute__((used, section("_fdt_node"))) = { \
        .generate = fn \
    }
```

**Usage:**
```c
static int generate_my_node(void *fdt, void *cookie) {
    fdt_begin_node(fdt, "my-device@1000");
    fdt_property_string(fdt, "compatible", "vendor,device");
    fdt_end_node(fdt);
    return 0;
}

DEFINE_FDT_NODE(my_device, generate_my_node);
```

### fdt_node_generate_all()

Generate all registered FDT nodes.

```c
void fdt_node_generate_all(void *fdt, void *cookie);
```

## Convenience Functions

### MMIO Operations

```c
// Start MMIO request
int driver_rpc_req_mmio_start(
    vso_rpc_t *rpc,
    unsigned int direction,
    unsigned int addr_space,
    unsigned int slot,
    seL4_Word addr,
    seL4_Word len,
    seL4_Word data
);

// Complete MMIO with response
int driver_rpc_ack_mmio_finish(
    vso_rpc_t *rpc,
    rpcmsg_t *msg,
    seL4_Word data
);
```

### VM Operations

```c
// Signal VM ready to start
int device_rpc_req_start_vm(vso_rpc_t *rpc);

// Register PCI device
int device_rpc_req_create_vpci_device(
    vso_rpc_t *rpc,
    seL4_Word pcidev
);

// Configure MMIO region
int device_rpc_req_mmio_region_config(
    vso_rpc_t *rpc,
    uintptr_t gpa,
    size_t size,
    unsigned long flags
);
```

### IRQ Operations

```c
// Set IRQ line high
int device_rpc_req_set_irqline(vso_rpc_t *rpc, seL4_Word irq);

// Set IRQ line low
int device_rpc_req_clear_irqline(vso_rpc_t *rpc, seL4_Word irq);

// Pulse IRQ line
int device_rpc_req_pulse_irqline(vso_rpc_t *rpc, seL4_Word irq);
```

## Message Processing Macros

### for_each_driver_rpc_req()

Iterate over incoming driver RPC requests.

```c
rpcmsg_t *msg;
for_each_driver_rpc_req(msg, &rpc) {
    // Process msg
}
```

### for_each_driver_rpc_resp()

Iterate over driver RPC responses.

```c
rpcmsg_t *msg;
int id;
for_each_driver_rpc_resp(msg, id, &rpc) {
    // Process response msg with id
}
```

### for_each_device_event()

Iterate over device events.

```c
rpcmsg_t msg;
for_each_device_event(msg, &rpc) {
    // Process event msg
}
```

## Bit Field Macros

### Message Field Access

```c
// Get operation code from mr0
int op = QEMU_OP(msg->mr0);

// Get MMIO parameters
int slot = BIT_FIELD_GET(mr0, RPC_MR0_MMIO_SLOT);
int dir = BIT_FIELD_GET(mr0, RPC_MR0_MMIO_DIRECTION);
int len = BIT_FIELD_GET(mr0, RPC_MR0_MMIO_LENGTH);
int as = BIT_FIELD_GET(mr0, RPC_MR0_MMIO_ADDR_SPACE);
```

### Message Field Construction

```c
seL4_Word mr0 = 0;
mr0 = BIT_FIELD_SET(mr0, RPC_MR0_OP, QEMU_OP_MMIO);
mr0 = BIT_FIELD_SET(mr0, RPC_MR0_MMIO_DIRECTION, RPC_MR0_MMIO_DIRECTION_WRITE);
mr0 = BIT_FIELD_SET(mr0, RPC_MR0_MMIO_LENGTH, 4);
mr0 = BIT_FIELD_SET(mr0, RPC_MR0_MMIO_ADDR_SPACE, AS_GLOBAL);
```

## Type Definitions

### vso_rpc_id_t

RPC endpoint role.

```c
typedef enum vso_rpc_id {
    vso_rpc_driver = 0,     // VMM side (initiates requests)
    vso_rpc_device_km,      // Device kernel module
    vso_rpc_device,         // Device userspace (QEMU)
} vso_rpc_id_t;
```

### Address Space

```c
#define AS_GLOBAL           MASK(RPC_MR0_MMIO_ADDR_SPACE_WIDTH)  // Global MMIO
#define AS_PCIDEV(__slot)   (__slot)  // PCI device slot
```

### IRQ Levels

```c
#define RPC_IRQ_CLR     0   // Clear (level low)
#define RPC_IRQ_SET     1   // Set (level high)
#define RPC_IRQ_PULSE   2   // Pulse (edge)
```

## Source Files

| File | Description |
|------|-------------|
| `include/sel4/rpc.h` | RPC structures and API |
| `include/sel4/rpc_queue.h` | Queue implementation |
| `include/tii/io_proxy.h` | I/O proxy API |
| `include/tii/irq_line.h` | IRQ line API |
| `include/tii/shared_irq_line.h` | Shared IRQ API |
| `include/tii/gicv2m.h` | GICv2m API |
| `include/tii/fdt.h` | FDT API |

## Related Documentation

- [RPC Opcodes](rpc-opcodes.md)
- [RPC Protocol](../architecture/rpc-protocol.md)
- [I/O Proxy](../components/io-proxy.md)
