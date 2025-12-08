# QEMU Backend

This document describes the QEMU seL4 accelerator that runs in device VMs to provide virtio backends.

## Overview

QEMU runs inside the device VM with a custom accelerator:

```mermaid
graph TB
    subgraph "Device VM (Linux)"
        subgraph "QEMU Process"
            ACCEL[seL4 Accelerator]
            VIRTIO[virtio Backends]
            VSORPC[vso_rpc Client]
        end
        UIO[/dev/uio* Devices]
        KERNEL[Linux Kernel]
    end

    subgraph "seL4 VMM"
        IOPROXY[I/O Proxy]
        RPCQ[RPC Queues]
    end

    VIRTIO --> ACCEL
    ACCEL --> VSORPC
    VSORPC --> UIO
    UIO --> KERNEL
    KERNEL --> RPCQ
    RPCQ --> IOPROXY
```

## QEMU seL4 Accelerator

### Architecture

The seL4 accelerator replaces KVM/TCG:

| Component | Standard QEMU | seL4 QEMU |
|-----------|--------------|-----------|
| CPU emulation | KVM/TCG | None (passthrough) |
| Memory | Host memory | Shared dataports |
| I/O | Host devices | RPC to VMM |
| Interrupts | KVM irqfd | RPC SET_IRQ |

### Source Files

| File | Description |
|------|-------------|
| `accel/sel4/sel4-all.c` | Main accelerator |
| `accel/sel4/sel4-vpci.c` | Virtual PCI support |
| `hw/arm/virt-sel4.c` | ARM Virt machine for seL4 |
| `hw/intc/sel4_intc.c` | Interrupt controller |
| `include/sysemu/sel4.h` | seL4 headers |

### Initialization

```c
// accel/sel4/sel4-all.c
static int sel4_accel_init(MachineState *ms) {
    // Open UIO device for shared memory
    sel4_state.uio_fd = open("/dev/uio0", O_RDWR);

    // Map shared memory regions
    sel4_state.iobuf = mmap(...);
    sel4_state.memdev = mmap(...);

    // Initialize vso_rpc
    vso_rpc_init(&sel4_state.rpc,
                 sel4_state.iobuf,
                 sel4_state.memdev);

    // Start virtio processing thread
    pthread_create(&sel4_state.virtio_thread,
                   NULL, virtio_thread_fn, NULL);

    return 0;
}
```

## vso_rpc Library

### Client-Side Usage

```c
// In QEMU seL4 accelerator
#include <sel4/rpc.h>

static vso_rpc_t rpc;

// Send MMIO response
void sel4_mmio_complete(uint64_t addr, uint64_t data, int size) {
    rpcmsg_t msg = {
        .mr0 = MAKE_MR0(QEMU_OP_MMIO, slot, DIR_READ, AS_GLOBAL, size),
        .mr1 = addr,
        .mr2 = data,
    };
    rpc_queue_enqueue(&rpc.driver_rpc_resp, &msg);
    doorbell_ring(&rpc);
}

// Send interrupt
void sel4_set_irq(int irq, int level) {
    rpcmsg_t msg = {
        .mr0 = MAKE_MR0(QEMU_OP_SET_IRQ, 0, 0, 0, 0),
        .mr1 = irq,
        .mr2 = level,
    };
    rpc_queue_enqueue(&rpc.device_event, &msg);
    doorbell_ring(&rpc);
}
```

### RPC Processing Loop

```c
static void *virtio_thread_fn(void *arg) {
    while (1) {
        // Wait for doorbell
        doorbell_wait(&rpc);

        // Process incoming requests
        rpcmsg_t msg;
        while (rpc_queue_dequeue(&rpc.driver_rpc_req, &msg)) {
            int op = RPC_OP(msg.mr0);

            switch (op) {
            case QEMU_OP_MMIO:
                handle_mmio_request(&msg);
                break;
            default:
                // Unknown opcode
                break;
            }
        }
    }
    return NULL;
}
```

## MMIO Handling

### Request Processing

```c
static void handle_mmio_request(rpcmsg_t *msg) {
    int slot = RPC_MMIO_SLOT(msg->mr0);
    int dir = RPC_MMIO_DIR(msg->mr0);
    int as = RPC_ADDR_SPACE(msg->mr0);
    int len = RPC_MMIO_LEN(msg->mr0);
    uint64_t addr = msg->mr1;
    uint64_t data = msg->mr2;

    if (dir == DIR_WRITE) {
        // Handle MMIO write
        if (as == AS_GLOBAL) {
            cpu_physical_memory_write(addr, &data, len);
        } else {
            // PCI device MMIO
            pci_device_write(as, addr, data, len);
        }
        // Send completion
        sel4_mmio_complete(addr, 0, len);
    } else {
        // Handle MMIO read
        if (as == AS_GLOBAL) {
            cpu_physical_memory_read(addr, &data, len);
        } else {
            data = pci_device_read(as, addr, len);
        }
        // Send response with data
        sel4_mmio_complete(addr, data, len);
    }
}
```

## PCI Device Registration

### Device Creation

```c
// When QEMU creates a virtio-blk device
static void virtio_blk_realize(DeviceState *dev, Error **errp) {
    VirtIOBlock *s = VIRTIO_BLK(dev);

    // Standard virtio initialization
    virtio_init(VIRTIO_DEVICE(s), ...);

    // Register with seL4 VMM
    sel4_register_pci_device(
        PCI_DEVICE(dev)->devfn,
        PCI_VENDOR_ID_REDHAT_QUMRANET,
        PCI_DEVICE_ID_VIRTIO_BLOCK,
        PCI_CLASS_STORAGE_SCSI
    );
}
```

### Registration Message

```c
void sel4_register_pci_device(int devfn, uint16_t vendor,
                               uint16_t device, uint32_t class) {
    rpcmsg_t msg = {
        .mr0 = MAKE_MR0(QEMU_OP_REGISTER_PCI_DEV, 0, 0, 0, 0),
        .mr1 = device | (vendor << 16),
        .mr2 = 0,  // subsystem
        .mr3 = class,
    };
    rpc_queue_enqueue(&rpc.device_event, &msg);
    doorbell_ring(&rpc);

    // Wait for slot assignment
    rpcmsg_t resp;
    while (!rpc_queue_dequeue(&rpc.driver_rpc_resp, &resp)) {
        doorbell_wait(&rpc);
    }

    int assigned_slot = resp.mr1;
    // Update device with assigned slot
}
```

## Interrupt Injection

### virtio Interrupt

```c
// When virtio device needs to signal guest
static void virtio_notify(VirtIODevice *vdev, VirtQueue *vq) {
    // Get IRQ number for this device
    int irq = pci_get_irq(PCI_DEVICE(vdev));

    // Send to VMM
    sel4_set_irq(irq, RPC_IRQ_PULSE);
}
```

### MSI Support

```c
static void virtio_msi_notify(VirtIODevice *vdev, VirtQueue *vq) {
    int vector = vq->vector;

    if (msix_enabled(PCI_DEVICE(vdev))) {
        // MSI-X notification
        uint32_t data = msix_get_data(PCI_DEVICE(vdev), vector);
        sel4_msi_notify(data);
    }
}
```

## ARM Virt-seL4 Machine

### Machine Definition

```c
// hw/arm/virt-sel4.c
static void virt_sel4_machine_init(MachineClass *mc) {
    mc->desc = "ARM Virtual Machine for seL4";
    mc->init = virt_sel4_init;
    mc->max_cpus = 1;  // Single vCPU
    mc->default_ram_size = 512 * MiB;
}

static void virt_sel4_init(MachineState *machine) {
    // Initialize memory from shared region
    sel4_region_t region;
    sel4_region_get(&region);

    memory_region_init_ram_from_fd(
        &ram, NULL, "ram",
        region.size, true, region.fd, 0, NULL);

    // Create PCIe host
    create_pcie(machine);

    // No CPU creation - passthrough to VMM
}
```

## Guest Image Configuration

### QEMU Command Line

In device VM, QEMU is started with:

```bash
qemu-system-aarch64 \
    -machine virt-sel4 \
    -accel sel4 \
    -m 512 \
    -nographic \
    -device virtio-blk-pci,drive=hd0 \
    -drive file=/var/lib/virt/images/user-vm.qcow2,id=hd0,format=qcow2
```

### Systemd Service

```ini
# /etc/systemd/system/qemu-virtio.service
[Unit]
Description=QEMU virtio backend
After=network.target

[Service]
ExecStart=/usr/bin/qemu-system-aarch64 \
    -machine virt-sel4 \
    -accel sel4 \
    ...
Restart=always

[Install]
WantedBy=multi-user.target
```

## Supported Devices

| Device | QEMU Name | Description |
|--------|-----------|-------------|
| Block | virtio-blk-pci | Disk storage |
| Network | virtio-net-pci | Networking |
| Console | virtio-serial-pci | Serial console |
| 9P | virtio-9p-pci | Filesystem sharing |
| RNG | virtio-rng-pci | Random number |

## Debugging

### QEMU Logs

```bash
# Enable QEMU tracing
qemu-system-aarch64 ... \
    -d guest_errors,unimp \
    -D /tmp/qemu.log
```

### RPC Debugging

```c
// Enable RPC tracing
#define RPC_TRACE 1

void rpc_trace(const char *fmt, ...) {
#if RPC_TRACE
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
#endif
}
```

## Source Files

| File | Description |
|------|-------------|
| `vm-images/.../qemu/accel/sel4/` | seL4 accelerator |
| `vm-images/.../qemu/hw/arm/virt-sel4.c` | ARM machine |
| `include/sel4/rpc.h` | RPC definitions |

## Related Documentation

- [Virtio Architecture](../architecture/virtio-architecture.md)
- [RPC Protocol](../architecture/rpc-protocol.md)
- [Guest-Side Components](guest-side-components.md)
