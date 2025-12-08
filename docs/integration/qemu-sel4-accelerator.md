# QEMU seL4 Accelerator

This document describes the QEMU seL4 accelerator (`accel/sel4/`) that enables QEMU to run virtio backends in a device VM, communicating with the seL4 VMM via RPC.

## Overview

The seL4 accelerator is a QEMU accelerator (like KVM or TCG) that:

- Uses `/dev/sel4` instead of `/dev/kvm`
- Maps shared memory for guest RAM and RPC queues
- Handles MMIO requests from the VMM via RPC
- Injects interrupts (including MSI) to the driver VM
- Supports vhost via ioeventfd/irqfd

```mermaid
graph TB
    subgraph "QEMU Process"
        ACCEL[seL4 Accelerator]
        VIRTIO[virtio Device Models]
        PCI[seL4 PCI Host]
        MEM[Memory Listener]
        DEV[Device Listener]
    end

    subgraph "kmod-sel4-virt"
        KMOD[/dev/sel4]
        IOBUF[IOBUF mmap]
        RAM[RAM mmap]
        EVENT[Event BAR mmap]
    end

    subgraph "seL4 VMM"
        RPC[RPC Handler]
        PROXY[I/O Proxy]
    end

    ACCEL -->|ioctl| KMOD
    ACCEL -->|mmap| RAM
    ACCEL -->|mmap| IOBUF
    ACCEL -->|mmap| EVENT

    VIRTIO --> ACCEL
    PCI --> ACCEL
    MEM -->|ioeventfd| KMOD
    DEV -->|MSI setup| ACCEL

    IOBUF <-->|RPC messages| RPC
    EVENT -->|doorbell| PROXY
```

## Source Location

```
~/tii-sel4/sources/qemu-sel4-virtio/accel/sel4/
├── sel4-all.c      # Main accelerator implementation
├── sel4-vpci.c     # seL4 PCI host bridge
└── meson.build     # Build configuration

# Headers
include/sysemu/sel4.h   # Public seL4 accelerator API
```

## Accelerator Architecture

### State Structure

```c
// From sel4-all.c
typedef struct SeL4State {
    AccelState parent_obj;

    int fd;      // /dev/sel4 file descriptor
    int vmfd;    // VM file descriptor

    struct {
        int fd;
        void *ptr;
    } maps[NUM_SEL4_MEM_MAP];  // Memory mappings

    vso_rpc_t rpc;             // RPC context

    MemoryListener mem_listener;  // For ioeventfd
    DeviceListener dev_listener;  // For MSI setup
} SeL4State;
```

### Memory Maps

| Index | Name | Size | Purpose |
|-------|------|------|---------|
| `SEL4_MEM_MAP_RAM` | Guest RAM | VM RAM size | Driver VM memory |
| `SEL4_MEM_MAP_IOBUF` | IOBUF | 2 pages | RPC queue shared memory |
| `SEL4_MEM_MAP_EVENT_BAR` | Event BAR | 1 page | Doorbell register |

## Initialization

### Accelerator Init

```c
// From sel4-all.c
static int sel4_init(MachineState *ms)
{
    SeL4State *s = SEL4_STATE(ms->accelerator);

    // 1. Open kernel module
    s->fd = open("/dev/sel4", O_RDWR);

    // 2. Create VM
    struct sel4_vm_params params = {
        .id = vmid,
        .ram_size = ms->ram_size,
    };
    s->vmfd = ioctl(s->fd, SEL4_CREATE_VM, &params);

    // 3. Map memory regions
    for (int i = 0; i < NUM_SEL4_MEM_MAP; i++) {
        s->maps[i].fd = ioctl(s->vmfd, SEL4_CREATE_IO_HANDLER, i);
        s->maps[i].ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, s->maps[i].fd, 0);
    }

    // 4. Setup guest RAM
    memory_region_init_ram_ptr(&ram_mr, OBJECT(ms), "virt.ram",
                               ms->ram_size, s->maps[SEL4_MEM_MAP_RAM].ptr);

    // 5. Initialize RPC
    vso_rpc_init(&s->rpc, vso_rpc_device,
                 s->maps[SEL4_MEM_MAP_IOBUF].ptr,
                 s2_fault_doorbell,
                 s->maps[SEL4_MEM_MAP_EVENT_BAR].ptr);

    // 6. Register memory listener (for ioeventfd)
    s->mem_listener.eventfd_add = sel4_io_ioeventfd_add;
    s->mem_listener.eventfd_del = sel4_io_ioeventfd_del;
    memory_listener_register(&s->mem_listener, &address_space_memory);

    // 7. Enable MSI support
    sel4_ext_msi_allowed = true;
    sel4_irqfds_allowed = true;

    if (sel4_ext_msi_enabled()) {
        device_listener_register(&s->dev_listener);
        msi_nonbroken = true;
        sel4_msi_via_irqfd_allowed = sel4_irqfds_enabled();
    }

    return 0;
}
```

### Doorbell Callback

```c
// Called when QEMU needs to notify VMM
static void s2_fault_doorbell(void *cookie)
{
    uint32_t *event_bar = cookie;
    // Write to event BAR triggers VMM notification
    event_bar[0] = 1;
}
```

## MMIO Handling

### virtio Thread

QEMU runs a dedicated thread to process MMIO requests from the VMM:

```c
// From sel4-all.c
static void *do_sel4_virtio(void *opaque)
{
    SeL4State *s = opaque;
    rpcmsg_t *msg;

    for (;;) {
        // Block until VMM has requests
        ioctl(s->vmfd, SEL4_WAIT_IO, 0);

        // Process all pending MMIO requests
        for_each_driver_rpc_req(msg, &s->rpc) {
            rpc_process(msg, s);
        }
    }
}

static int rpc_process(rpcmsg_t *msg, void *cookie)
{
    SeL4State *s = cookie;

    switch (QEMU_OP(msg->mr0)) {
    case QEMU_OP_MMIO:
        return handle_mmio(s, msg);
    default:
        fprintf(stderr, "Unknown op %lu\n", QEMU_OP(msg->mr0));
        return -1;
    }
}
```

### MMIO Request Processing

```c
static inline int handle_mmio(SeL4State *s, rpcmsg_t *req)
{
    seL4_Word dir = BIT_FIELD_GET(req->mr0, RPC_MR0_MMIO_DIRECTION);
    seL4_Word as = BIT_FIELD_GET(req->mr0, RPC_MR0_MMIO_ADDR_SPACE);
    seL4_Word len = BIT_FIELD_GET(req->mr0, RPC_MR0_MMIO_LENGTH);
    seL4_Word addr = req->mr1;
    seL4_Word data = req->mr2;

    qemu_mutex_lock_iothread();

    if (as == AS_GLOBAL) {
        // Global MMIO (virtio MMIO regions)
        if (dir == SEL4_IO_DIR_READ) {
            address_space_read(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, &data, len);
        } else {
            address_space_write(&address_space_memory, addr,
                                MEMTXATTRS_UNSPECIFIED, &data, len);
        }
    } else {
        // PCI config space access
        sel4_pci_do_io(as, dir, addr, &data, len);
    }

    qemu_mutex_unlock_iothread();

    // Send response
    return driver_rpc_ack_mmio_finish(&s->rpc, req, data);
}
```

## MSI Support

### MSI Trigger

When a virtio device signals an interrupt via MSI:

```c
// From sel4-all.c
void sel4_msi_trigger(PCIDevice *dev, MSIMessage msg)
{
    assert(sel4_ext_msi_enabled());

    // Extract GSI (SPI number) from MSI data
    uint32_t irq = sel4_msi_data_to_gsi(msg.data);

    // Pulse the IRQ via RPC
    sel4_set_irq(irq, true);
    sel4_set_irq(irq, false);
}

static uint32_t sel4_msi_data_to_gsi(uint32_t data)
{
    // Direct mapping: MSI data lower 16 bits = SPI number
    return data & 0xffff;
}
```

### IRQ Injection via RPC

```c
void sel4_set_irq(unsigned int irq, bool state)
{
    SeL4State *s = SEL4_STATE(current_accel());

    if (state) {
        device_rpc_req_set_irqline(&s->rpc, irq);
    } else {
        device_rpc_req_clear_irqline(&s->rpc, irq);
    }
}
```

### MSI Route Setup

```c
// Called when QEMU sets up MSI for a device
int sel4_add_msi_route(int vector, PCIDevice *dev)
{
    MSIMessage msg = {0, 0};

    if (pci_available && dev) {
        msg = pci_get_msi_message(dev, vector);
    }

    // Direct GSI mapping
    return sel4_msi_data_to_gsi(msg.data);
}
```

### Device Listener for MSI

```c
// Automatically set up MSI trigger for PCI devices
static void sel4_dev_realize(DeviceListener *listener, DeviceState *dev)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_PCI_DEVICE)) {
        PCIDevice *pci_dev = PCI_DEVICE(dev);
        pci_dev->msi_trigger = sel4_msi_trigger;
    }
}
```

## ioeventfd Integration

### Registration

When QEMU (or vhost) registers a memory region with eventfd:

```c
static void sel4_io_ioeventfd_add(MemoryListener *listener,
                                  MemoryRegionSection *section,
                                  bool match_data, uint64_t data,
                                  EventNotifier *e)
{
    SeL4State *s = container_of(listener, SeL4State, mem_listener);
    int fd = event_notifier_get_fd(e);

    struct sel4_ioeventfd_config config = {
        .fd = fd,
        .addr = section->offset_within_address_space,
        .addr_space = AS_GLOBAL,
        .len = int128_get64(section->size),
        .data = match_data ? data : 0,
        .flags = match_data ? SEL4_IOEVENTFD_FLAG_DATAMATCH : 0,
    };

    ioctl(s->vmfd, SEL4_IOEVENTFD, &config);
}
```

## irqfd Integration

### Registration

For vhost MSI-X interrupt injection:

```c
int sel4_add_irqfd_notifier(EventNotifier *n, EventNotifier *rn, int virq)
{
    SeL4State *s = SEL4_STATE(current_accel());

    struct sel4_irqfd_config irqfd = {
        .fd = event_notifier_get_fd(n),
        .virq = virq,  // SPI number
        .flags = 0,
    };

    return ioctl(s->vmfd, SEL4_IRQFD, &irqfd);
}

int sel4_remove_irqfd_notifier(EventNotifier *n, int virq)
{
    SeL4State *s = SEL4_STATE(current_accel());

    struct sel4_irqfd_config irqfd = {
        .fd = event_notifier_get_fd(n),
        .virq = virq,
        .flags = SEL4_IRQFD_FLAG_DEASSIGN,
    };

    return ioctl(s->vmfd, SEL4_IRQFD, &irqfd);
}
```

## PCI Host Bridge

### seL4 PCI Host

QEMU provides a custom PCI host bridge for the seL4 vPCI bus:

```c
// From sel4-vpci.c
typedef struct SeL4PCIHost {
    PCIExpressHost parent_obj;

    MemoryRegion mmio;
    MemoryRegion ioport;
    qemu_irq irq[SEL4_VPCI_INTERRUPTS];
    int irq_num[SEL4_VPCI_INTERRUPTS];
} SeL4PCIHost;
```

### Device Registration

When a PCI device is plugged:

```c
static void sel4_pcihost_device_plug(HotplugHandler *hotplug_dev,
                                     DeviceState *dev, Error **errp)
{
    sel4_register_pci_device(PCI_DEVICE(dev));
}

void sel4_register_pci_device(PCIDevice *d)
{
    SeL4State *s = SEL4_STATE(current_accel());
    struct sel4_vpci_device vpcidev = {
        .pcidev = pci_dev_count,
    };

    // Register with VMM
    ioctl(s->vmfd, SEL4_CREATE_VPCI_DEVICE, &vpcidev);
    pci_devs[pci_dev_count++] = d;
}
```

## VM Lifecycle

### Starting the VM

```c
static void sel4_change_state_handler(void *opaque, bool running, RunState state)
{
    SeL4State *s = opaque;

    if (running) {
        // Signal VMM that device VM is ready
        ioctl(s->vmfd, SEL4_START_VM, 0);
    }
}
```

### Bootargs Parsing

QEMU reads driver VM configuration from kernel bootargs:

```c
// Format: uservm=id,ram_base,ram_size,pcie_mmio_base,pcie_mmio_size
static int parse_kernel_bootargs(void)
{
    // Parse /proc/cmdline for uservm= parameter
    // Sets: uservm_ram_base, uservm_ram_size,
    //       uservm_pcie_mmio_base, uservm_pcie_mmio_size
}

MemMapEntry sel4_region_get(SeL4MemoryRegion region)
{
    switch (region) {
    case SEL4_REGION_RAM:
        return (MemMapEntry){uservm_ram_base, uservm_ram_size};
    case SEL4_REGION_PCIE_MMIO:
        return (MemMapEntry){uservm_pcie_mmio_base, uservm_pcie_mmio_size};
    // ...
    }
}
```

## QEMU Command Line

### Basic Usage

```bash
qemu-system-aarch64 \
    -machine virt-sel4 \
    -accel sel4 \
    -m 512 \
    -nographic \
    -device virtio-blk-pci,drive=hd0 \
    -drive file=/path/to/disk.qcow2,id=hd0,format=qcow2
```

### With vhost-net

```bash
qemu-system-aarch64 \
    -machine virt-sel4 \
    -accel sel4 \
    -m 512 \
    -netdev tap,id=net0,vhost=on,queues=4 \
    -device virtio-net-pci,netdev=net0,mq=on,vectors=10
```

### Environment Variables

| Variable | Description |
|----------|-------------|
| `VMID` | Driver VM ID (default: 1) |

## Feature Flags

```c
bool sel4_allowed;              // Accelerator enabled
bool sel4_ext_vpci_bus_allowed; // vPCI bus support
bool sel4_ext_msi_allowed;      // MSI support
bool sel4_irqfds_allowed;       // irqfd support
bool sel4_msi_via_irqfd_allowed;// MSI via irqfd (vhost)
```

## Debugging

### Enable QEMU Tracing

```bash
qemu-system-aarch64 ... \
    -d guest_errors,unimp \
    -D /tmp/qemu.log
```

### RPC Debug Output

The accelerator prints to stderr for errors:
```c
fprintf(stderr, "sel4: create VM failed: %d %s\n", -rc, strerror(-rc));
fprintf(stderr, "%s failed, addr=0x%lx, dir=%lu\n", __func__, addr, dir);
```

### Debug Ring Buffer

```c
// tii_printf() writes to QEMU ring buffer
void tii_printf(const char *fmt, ...);
```

## Source Files

| File | Description |
|------|-------------|
| `accel/sel4/sel4-all.c` | Main accelerator |
| `accel/sel4/sel4-vpci.c` | PCI host bridge |
| `include/sysemu/sel4.h` | Public API |

## Related Documentation

- [kmod-sel4-virt](kmod-sel4-virt.md) - Kernel module (other side of bridge)
- [vhost Acceleration](../architecture/vhost-acceleration.md) - Why MSI matters
- [RPC Protocol](../architecture/rpc-protocol.md) - RPC message format
- [QEMU Backend](qemu-backend.md) - High-level QEMU overview
