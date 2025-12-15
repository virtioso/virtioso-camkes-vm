# SDEI RAS Error Handling: ATF to Linux Flow

This document describes the complete flow of RAS (Reliability, Availability, Serviceability) errors from ARM Trusted Firmware (ATF) through SDEI (Software Delegated Exception Interface) to the Linux kernel.

## Overview

When a RAS error occurs on Tegra Orin (T234), the following chain of events occurs:

```
Hardware RAS Error
    ↓
ATF RAS Handler (EL3)
    ↓
SDEI Event Dispatch
    ↓
Linux SDEI Entry (Assembly)
    ↓
Linux SDEI Handler (C)
    ↓
GHES Callback (NMI context)
    ↓
IRQ Work Queue
    ↓
Deferred Error Processing
    ↓
RAS Tracepoint / User-space Tools
```

## Part 1: ATF Side - RAS Detection and SDEI Dispatch

### 1.1 RAS Error Detection

**Source:** `plat/nvidia/tegra/soc/t234/plat_ras.c`

When a RAS error occurs, ATF receives it via:
- **RAS Interrupt:** Through the Interrupt Handler (IH) - handled by `tegra234_ras_handler()`
- **External Abort:** Through `plat_ea_handler()` or `plat_handle_el3_ea()`

### 1.2 RAS Handler Entry Point

**Function:** `tegra234_ras_handler()` (line 454-608)

```c
static int tegra234_ras_handler(uint32_t intr_raw, uint32_t flags,
        void *handle, void *cookie)
{
    // 1. Read IH interrupt status
    uint64_t intr_status = ih_read_ras_intstatus(plat_my_core_pos());

    // 2. Determine which RAS node group triggered
    if ((intr_status & TEGRA234_IH_STATUS_MASK_CMU) != 0U)
        selected = tegra234_ras_records_common;
    // ... other groups

    // 3. For each error record, probe and handle
    while (num_records-- > 0U) {
        ret = selected->probe(selected, &probe_data);
        if (ret != 0) {
            ret = selected->handler(selected, probe_data, &err_data);

            // 4. Dispatch SDEI event to Linux
            ret = sdei_dispatch_event(TEGRA_SDEI_EP_EVENT_0 + plat_my_core_pos());
        }
    }
}
```

### 1.3 Error Information Captured

**Function:** `tegra234_ras_node_handler()` (line 276-413)

ATF reads and logs the following from RAS error records:

| Register | Content |
|----------|---------|
| `ERR_STATUS(0)` | Error status (V, UE, CE, OF, DE, MV, AV bits) |
| `ERR_ADDR(0)` | Fault address |
| `ERR_MISC0-3(0)` | Implementation-specific error details |
| `IERR` | Implementation Error code |
| `SERR` | Standard Error code (0-21) |

**SERR Codes (selected):**
- 0: No error
- 1: Implementation defined
- 4: Assertion failure
- 6-7: Cache (associative memory) errors
- 8-9: TLB errors
- 13-15: Software faults (illegal address/access/state)
- 18: Error response from slave
- (see Part 8.6 for complete table)

### 1.3.1 ARM RAS ADDR Register Encoding

**CRITICAL**: The ADDR field in RAS error records is NOT a raw physical address. The upper bits contain status flags!

| Bits | Field | Description |
|------|-------|-------------|
| **63** | **NS** | Non-secure attribute (1 = non-secure access) |
| 62 | SI | Secure Incorrect (1 = NS field may be wrong) |
| 61 | AI | Address Incorrect (1 = PADDR may be wrong) |
| 60 | VA | Virtual Address (1 = address is VA, not PA) |
| 59:56 | RES0 | Reserved |
| **55:0** | **PADDR** | **Actual physical address** |

**Example**: `ADDR = 0x8000000000000000`
- Bit 63 (NS) = 1 → Non-secure access
- Bits 55:0 = 0x0 → **Physical address is ZERO (NULL pointer access!)**

This is NOT "bit 63 set = invalid address". It means **PA=0x0 accessed from non-secure world**.

### 1.4 SDEI Event Dispatch

**Function:** `sdei_dispatch_event()` (services/std_svc/sdei/sdei_intr_mgmt.c:592-663)

```c
int sdei_dispatch_event(int ev_num)
{
    // 1. Check if SDEI events are masked
    if (sdei_pe_masked())
        return -1;

    // 2. Find event mapping
    map = sdei_event_map(ev_num);

    // 3. Validate event state (must be registered + enabled)

    // 4. Setup dispatch context with handler arguments
    setup_ns_dispatch(map, se, disp_ctx);

    // 5. Dispatch synchronously to Non-Secure world
    begin_sdei_synchronous_dispatch(disp_ctx);
}
```

### 1.5 Tegra SDEI Event Numbers

**Source:** `plat/nvidia/tegra/include/platform_def.h`

| Event Type | Numbers | Priority | Purpose |
|------------|---------|----------|---------|
| Event 0 | 0 | Normal | Private signalable (SGI) |
| Dynamic Private | 100-102 | Normal | Runtime-bindable |
| Dynamic Shared | 200-202 | Normal | Runtime-bindable |
| **Explicit Private (RAS)** | **300-311** | **Critical** | Per-CPU RAS events |

RAS errors use events 300-311 (one per CPU core).

### 1.6 Context Passed to Linux

**Function:** `setup_ns_dispatch()` (line 312-349)

When dispatching to Linux, ATF sets up:

| Register | Value |
|----------|-------|
| x0 | Event number (300-311) |
| x1 | Handler argument (from registration) |
| x2 | Interrupted PC (ELR_EL3) |
| x3 | Interrupted PSTATE (SPSR_EL3) |

Additionally, x0-x17 are saved and retrievable via `SDEI_EVENT_CONTEXT` SMC.

### 1.7 Critical Observation

**RAS error details (status, address, MISC registers) are logged to console by ATF but NOT passed through SDEI.** The OS receives only:
- Event number
- Interrupted context (PC, PSTATE)
- Saved registers via SMC query

To get actual error details, Linux must read from a shared memory region (GHES error status block) that firmware populates.

---

## Part 2: Linux Kernel - SDEI Reception

### 2.1 SDEI Initialization

**Source:** `drivers/firmware/arm_sdei.c`

```c
void __init sdei_init(void)
{
    // 1. Register platform driver
    platform_driver_register(&sdei_driver);

    // 2. Probe detects SMC/HVC conduit
    // 3. Gets entry point from arch code
    // 4. Registers CPU hotplug and PM notifiers
}
```

### 2.2 ARM64 Entry Point Selection

**Source:** `arch/arm64/kernel/sdei.c:200-236`

```c
unsigned long sdei_arch_get_entry_point(int conduit)
{
    // 1. Check hypervisor support
    if (is_hyp_mode_available() && !is_kernel_in_hyp_mode())
        return 0;  // SDEI not supported under hypervisor

    // 2. Allocate SDEI stacks (normal + critical)
    init_sdei_stacks();

    // 3. Return entry point
    if (arm64_kernel_unmapped_at_el0())
        return TRAMP_VALIAS + offset(__sdei_asm_entry_trampoline);
    else
        return (unsigned long)__sdei_asm_handler;
}
```

### 2.3 Assembly Entry Point

**Source:** `arch/arm64/kernel/entry.S:938-1096`

```asm
SYM_CODE_START(__sdei_asm_handler)
    // Arguments from firmware:
    // x0 = event_num
    // x1 = struct sdei_registered_event *
    // x2 = interrupted PC
    // x3 = interrupted PSTATE

    // 1. Save all registers to sdei_registered_event
    stp x2, x3, [x1, #SDEI_EVENT_INTREGS + S_PC]
    stp x4, x5, [x1, #SDEI_EVENT_INTREGS + 16 * 2]
    // ... save x6-x29 ...

    // 2. Mark as active event
    ldrb w4, [x19, #SDEI_EVENT_PRIORITY]
    str x19, [x5]  // sdei_active_normal_event or sdei_active_critical_event

    // 3. Switch to SDEI stack if needed

    // 4. Call C handler
    bl __sdei_handler

    // 5. Return to firmware via SDEI_EVENT_COMPLETE
    br x5
SYM_CODE_END(__sdei_asm_handler)
```

### 2.4 C Handler Bridge

**Source:** `arch/arm64/kernel/sdei.c:244-299`

```c
unsigned long __kprobes do_sdei_event(struct pt_regs *regs,
                                     struct sdei_registered_event *arg)
{
    // 1. Retrieve clobbered registers (x0-x3) from firmware
    for (i = 0; i < clobbered_registers; i++)
        sdei_api_event_context(i, &regs->regs[i]);

    // 2. Call registered callback
    err = sdei_event_handler(regs, arg);

    // 3. Determine return: original location or IRQ vector
}
```

### 2.5 Event Handler Dispatch

**Source:** `drivers/firmware/arm_sdei.c:1084-1096`

```c
int sdei_event_handler(struct pt_regs *regs,
                      struct sdei_registered_event *arg)
{
    u32 event_num = arg->event_num;

    // Call registered callback (GHES for RAS)
    err = arg->callback(event_num, regs, arg->callback_arg);
}
```

---

## Part 3: GHES - Generic Hardware Error Source

### 3.1 GHES Registration for SDEI

**Source:** `drivers/acpi/apei/ghes.c:1264-1375`

```c
ghes_probe(platform_device)
{
    // For SDEI notification type:
    case ACPI_HEST_NOTIFY_SOFTWARE_DELEGATED:
        sdei_register_ghes(ghes,
            ghes_sdei_normal_callback,
            ghes_sdei_critical_callback);
}
```

### 3.2 GHES SDEI Callbacks

**Source:** `drivers/acpi/apei/ghes.c:1219-1245`

```c
// Normal priority callback
static int ghes_sdei_normal_callback(u32 event_num, struct pt_regs *regs,
                                    void *arg)
{
    struct ghes *ghes = arg;
    __ghes_sdei_callback(ghes, FIX_APEI_GHES_SDEI_NORMAL);
    irq_work_queue(&ghes_proc_irq_work);  // Defer to IRQ context
}

// Critical priority callback
static int ghes_sdei_critical_callback(u32 event_num, struct pt_regs *regs,
                                      void *arg)
{
    __ghes_sdei_callback(ghes, FIX_APEI_GHES_SDEI_CRITICAL);
}
```

### 3.3 Error Queueing (NMI Context)

**Source:** `drivers/acpi/apei/ghes.c:1028-1090`

```c
static int ghes_in_nmi_queue_one_entry(struct ghes *ghes,
                                       enum fixed_addresses fixmap_idx)
{
    // 1. Read error status from firmware error block
    __ghes_read_estatus(estatus, buf_paddr, fixmap_idx, len);

    // 2. Check severity - panic if fatal
    sev = ghes_severity(estatus->error_severity);
    if (sev >= GHES_SEV_PANIC)
        __ghes_panic(ghes, estatus, buf_paddr, fixmap_idx);

    // 3. Add to linked list for deferred processing
    llist_add(&estatus_node->llnode, &ghes_estatus_llist);
}
```

### 3.4 Deferred Processing (IRQ Context)

**Source:** `drivers/acpi/apei/ghes.c:854-877`

```c
static int ghes_proc(struct ghes *ghes)
{
    // 1. Read error status
    rc = ghes_read_estatus(ghes, estatus, &buf_paddr, FIX_APEI_GHES_IRQ);

    // 2. Check for panic severity
    if (ghes_severity(estatus->error_severity) >= GHES_SEV_PANIC)
        __ghes_panic(...);

    // 3. Log error (with rate limiting)
    ghes_print_estatus(NULL, ghes->generic, estatus);

    // 4. Process error sections
    ghes_do_proc(ghes, estatus);

    // 5. Clear error status
    ghes_clear_estatus(...);
}
```

### 3.5 ARM Hardware Error Handling

**Source:** `drivers/acpi/apei/ghes.c:486-528`

```c
static bool ghes_handle_arm_hw_error(struct acpi_hest_generic_data *gdata,
                                     int sev)
{
    struct cper_sec_proc_arm *err = acpi_hest_get_payload(gdata);

    // 1. Log via RAS tracepoint
    log_arm_hw_error(err);

    // 2. For each error in section
    for (i = 0; i < err->err_info_num; i++) {
        struct cper_arm_err_info *err_info = ...

        // Handle cache errors with physical address
        if (is_cache && has_pa)
            ghes_do_memory_failure(err_info->physical_fault_addr, 0);
    }
}
```

---

## Part 4: Error Severity and Actions

### 4.1 Severity Levels

| GHES Severity | Value | Action |
|--------------|-------|--------|
| `GHES_SEV_NO` | 0 | No error |
| `GHES_SEV_CORRECTED` | 1 | Log only |
| `GHES_SEV_RECOVERABLE` | 2 | Log, may offline memory |
| `GHES_SEV_PANIC` | 3 | Kernel panic |

### 4.2 Actions Based on Error Type

| Error Type | Action |
|------------|--------|
| Cache error with PA | `ghes_do_memory_failure()` - may offline page |
| Memory error | `ghes_edac_report_mem_error()` - EDAC reporting |
| PCIe AER | `ghes_handle_aer()` - PCIe error handling |
| Unknown ARM error | `pr_warn()` - log only |

### 4.3 Memory Failure Handling

```c
void ghes_do_memory_failure(unsigned long pfn, int flags)
{
    // Attempt to offline the affected memory page
    // May kill process using the page
    memory_failure(pfn, flags);
}
```

---

## Part 5: Complete Call Chain

```
1. HARDWARE RAS ERROR
   └─ Detected by CPU/cache/interconnect

2. ATF RAS HANDLER (EL3)
   └─ plat_ras.c:tegra234_ras_handler() or plat_ea_handler()
      ├─ Read error records (status, addr, MISC0-3)
      ├─ Log error details to console
      ├─ Clear error records
      └─ Call sdei_dispatch_event(300 + core_id)

3. ATF SDEI DISPATCH
   └─ sdei_intr_mgmt.c:sdei_dispatch_event()
      ├─ Validate event state
      ├─ Setup handler args (x0=event, x1=arg, x2=PC, x3=PSTATE)
      └─ Switch to Non-Secure world

4. LINUX ASSEMBLY ENTRY
   └─ arch/arm64/kernel/entry.S:__sdei_asm_handler
      ├─ Save all registers
      ├─ Switch to SDEI stack
      └─ Call __sdei_handler

5. LINUX C HANDLER
   └─ arch/arm64/kernel/sdei.c:do_sdei_event()
      └─ drivers/firmware/arm_sdei.c:sdei_event_handler()
         └─ Call registered callback

6. GHES CALLBACK (NMI Context)
   └─ drivers/acpi/apei/ghes.c:ghes_sdei_critical_callback()
      ├─ Read error status from GHES block
      ├─ Check for panic severity
      ├─ Queue error for deferred processing
      └─ irq_work_queue()

7. DEFERRED PROCESSING (IRQ Context)
   └─ ghes_proc_in_irq()
      └─ ghes_proc()
         └─ ghes_do_proc()
            └─ ghes_handle_arm_hw_error()
               ├─ log_arm_hw_error() → RAS tracepoint
               └─ ghes_do_memory_failure() if applicable

8. USER-SPACE
   └─ rasdaemon listens to RAS tracepoints
      └─ Logs to /var/log/ras/
```

---

## Part 6: Key Observations for seL4

### 6.1 Why Linux Doesn't See Our RAS Errors

**Important:** Even when running Linux on Tegra Orin, RAS errors are NOT delivered through GHES:

1. **No HEST Table:** Tegra Orin doesn't provide ACPI HEST table (see Part 11)
2. **No GHES Registration:** Without HEST, Linux GHES driver doesn't register SDEI handlers
3. **SDEI Dispatch Fails:** ATF's `sdei_dispatch_event()` returns -1 (no handler)

When running seL4, there are additional issues:

1. **No SDEI Client:** seL4 doesn't implement SDEI protocol
2. **No Error Status Block:** No shared memory for CPER records

### 6.2 What ATF Does Log

Even without Linux SDEI handling, ATF logs:
- Error status register values
- Fault addresses (when valid)
- IERR/SERR codes
- EL2/EL3 context (PC, page tables, MMU registers)

### 6.3 RAS Error Address Decoding

**Important**: ADDR field `0x8000000000000000` does NOT mean "invalid address with bit 63 set".

Decoding (see Section 1.3.1):
- Bit 63 (NS) = 1 → Non-secure access
- Bits 55:0 = 0x0 → **Actual PA is 0x0 (NULL pointer!)**

The RAS errors we observed indicate **accesses to physical address zero** from non-secure world, likely caused by:

1. **NULL VTTBR Base:** Speculative table walker using NULL (0) as page table base
2. **Speculative Access:** CPU speculatively accessing PA=0x0 during TLB operations
3. **Page Table Walk:** Translation table walker dereferencing NULL pointer

### 6.4 Potential Solutions for seL4

1. **Implement Minimal SDEI Client:** Register handlers for events 300-311
2. **Shared Memory Error Block:** Allocate memory for ATF to report error details
3. **Suppress in ATF:** Current approach - handle/ignore in ATF without dispatch

---

## Part 7: Linux RAS Framework Details

### 7.1 CPER (Common Platform Error Record) Structures

**Source:** `include/linux/cper.h`

#### ARM Processor Error Section
```c
struct cper_sec_proc_arm {
    u32 validation_bits;         // CPER_ARM_VALID_* flags
    u16 err_info_num;            // Number of error info structures
    u16 context_info_num;        // Number of context info structures
    u32 section_length;          // Total section size
    u8 affinity_level;           // Processor hierarchy level
    u8 reserved[3];
    u64 mpidr;                   // Multiprocessor Affinity Register
    u64 midr;                    // Main ID Register (processor model)
    u32 running_state;           // 0 = offline, 1 = running
    u32 psci_state;              // PSCI power state
};
```

#### ARM Error Information
```c
struct cper_arm_err_info {
    u8 version;
    u8 length;
    u16 validation_bits;         // CPER_ARM_INFO_VALID_* flags
    u8 type;                     // CPER_ARM_*_ERROR (cache/TLB/bus/vendor)
    u16 multiple_error;          // Number of errors - 1
    u8 flags;                    // CPER_ARM_INFO_FLAGS_*
    u64 error_info;              // Detailed error information
    u64 virt_fault_addr;         // Virtual fault address
    u64 physical_fault_addr;     // Physical fault address
};
```

#### ARM Error Types
| Type | Value | Description |
|------|-------|-------------|
| `CPER_ARM_CACHE_ERROR` | 0 | Cache hierarchy error |
| `CPER_ARM_TLB_ERROR` | 1 | TLB error |
| `CPER_ARM_BUS_ERROR` | 2 | Bus error |
| `CPER_ARM_VENDOR_ERROR` | 3 | Vendor-specific error |

### 7.2 RAS Tracepoints

**Source:** `include/ras/ras_event.h`

#### ARM Event Tracepoint
```c
TRACE_EVENT(arm_event,
    TP_PROTO(const struct cper_sec_proc_arm *proc),
    TP_STRUCT__entry(
        __field(u64, mpidr)              // Processor affinity
        __field(u64, midr)               // Processor model
        __field(u32, running_state)      // Running/offline
        __field(u32, psci_state)         // Power state
        __field(u8, affinity)            // Affinity level
    ),
    TP_printk(
        "affinity level: %d; MPIDR: %016llx; MIDR: %016llx; "
        "running state: %d; PSCI state: %d",
        __entry->affinity, __entry->mpidr, __entry->midr,
        __entry->running_state, __entry->psci_state
    )
);
```

**Note:** The tracepoint exports processor-level information but NOT detailed error info or fault addresses. Those are processed separately in `ghes_handle_arm_hw_error()`.

### 7.3 Error Flow Through RAS Subsystem

```
Firmware detects error → generates CPER record
    ↓
GHES driver reads → ghes_do_proc() processes sections
    ↓
Section type matching → routes to appropriate handler
    ↓
For ARM errors → ghes_handle_arm_hw_error()
    ↓
Tracepoint fired → trace_arm_event() with processor info
    ↓
Kernel logging → cper_print_proc_arm() prints details
    ↓
User tools consume → via tracefs or kernel logs
    ↓
Recovery actions → memory failure handling if needed
```

### 7.4 User-Space Access Methods

1. **Ftrace/Tracefs:**
   - Location: `/sys/kernel/tracing/events/ras/`
   - Available: `arm_event/`, `non_standard_event/`, `mc_event/`, `aer_event/`

2. **Kernel Logging:**
   - Printed via `printk()` with rate limiting
   - Accessible via dmesg and syslog

3. **rasdaemon:**
   - Listens to RAS tracepoints
   - Logs to `/var/log/ras/`

---

## Part 8: Memory Failure and Error Recovery

### 8.1 Error Severity Decision Tree

```
Error received from firmware
    ↓
Check CPER_MEM_VALID_PA (physical address valid)?
    ├─ NO  → return false (skip processing)
    └─ YES → Get error severity
             ↓
         Check severity:
         ├─ CORRECTED + ERROR_THRESHOLD_EXCEEDED
         │  └─ Action: MF_SOFT_OFFLINE (soft offline page)
         ├─ RECOVERABLE (both overall & section-specific)
         │  └─ Action: memory_failure_queue() with flags=0
         └─ Other combinations
            └─ Action: Log only, no action taken
```

### 8.2 Memory Failure Handler Flow

**Source:** `mm/memory-failure.c`

```
memory_failure(pfn, flags)
    ↓
[1] Is sysctl_memory_failure_recovery enabled?
    └─ NO  → PANIC

[2] Check if PFN is valid (pfn_to_online_page)
    └─ INVALID → return -ENXIO ("memory outside kernel control")

[3] Check if page already poisoned (TestSetPageHWPoison)
    └─ YES → if MF_ACTION_REQUIRED: kill_accessing_process()

[4] Identify page state:
    ├─ RESERVED/KERNEL  → MF_IGNORED (no action)
    ├─ SLAB              → MF_IGNORED
    ├─ HUGE PAGE         → me_huge_page()
    ├─ DIRTY SWAPCACHE   → me_swapcache_dirty()
    ├─ CLEAN SWAPCACHE   → me_swapcache_clean()
    ├─ DIRTY LRU         → me_pagecache_dirty()
    ├─ CLEAN LRU         → me_pagecache_clean()
    └─ UNKNOWN           → MF_FAILED

[5] Process user mappings (hwpoison_user_mappings)
    ├─ DIRTY page or MF_MUST_KILL flag:
    │  └─ forcekill = true → kill_procs() sends SIGBUS
    └─ CLEAN page:
       └─ Just remove from mapping, optional signal
```

### 8.3 Correctable vs Uncorrectable Errors

#### Correctable Errors (GHES_SEV_CORRECTED)
- Action: `MF_SOFT_OFFLINE`
- Behavior: Page is **migrated** to healthy memory
- Process: Continues without termination
- Data: Preserved via migration

#### Uncorrectable Errors (GHES_SEV_RECOVERABLE)
- Action: `memory_failure_queue()`
- Behavior: Page is **poisoned** and process killed
- Signal: `SIGBUS` with `BUS_MCEERR_AR` or `BUS_MCEERR_AO`
- Data: Lost for dirty pages

### 8.4 Process Killing Decision

```c
// Kill decision in hwpoison_user_mappings()
if (PageDirty(page) || (flags & MF_MUST_KILL)) {
    forcekill = true;   // Must terminate - data would be lost
    kill_procs() → SIGBUS to all processes mapping the page
} else {
    forcekill = false;  // Can safely drop clean page
    // Just unmap, process continues
}
```

### 8.5 Physical Address Zero (PA=0x0) Handling

**IMPORTANT**: When ADDR = `0x8000000000000000`, the actual physical address is **0x0** (see Section 1.3.1 for ADDR encoding). Bit 63 is the NS flag, not part of the address.

If this error reached Linux GHES (which it doesn't on Tegra - see Part 11), the CPER record would contain `physical_fault_addr = 0x0`:

```c
ghes_do_memory_failure(0x0) {  // PA=0, not 0x8000000000000000!
    pfn = PHYS_PFN(0x0);  // = 0

    if (!pfn_valid(0)) {
        // PFN 0 is typically NOT valid (reserved for NULL detection)
        pr_warn_ratelimited(
            "Invalid address in generic error data: 0x0"
        );
        return false;  // Error discarded, no action
    }
}
```

**Result:** PA=0x0 (pfn 0) is typically invalid on most systems, so the error would be logged and discarded. This aligns with the theory that the errors are caused by NULL pointer accesses during speculative table walks.

### 8.6 Cache Error Detection vs SERR Codes

**CRITICAL DISTINCTION:** There are TWO completely separate error classification systems with NO standard mapping between them:

#### ARM RAS SERR Codes (ERR<n>STATUS register)
From ARM RAS architecture, read by ATF from hardware error records:

| SERR | Meaning |
|------|---------|
| 0 | No error |
| 1 | Implementation defined |
| 2 | Data value from non-associative memory |
| 3 | Implementation defined pin |
| 4 | Assertion failure |
| 5 | Error detected on internal data path |
| 6 | Data value from associative memory (cache data) |
| 7 | Address/control value from associative memory (cache tag) |
| 8 | Data value from a TLB |
| 9 | Address/control value from a TLB |
| 10-12 | Producer/external memory errors |
| 13 | Illegal address (software fault) |
| 14 | Illegal access (software fault) |
| 15 | Illegal state (software fault) |
| 16-21 | Internal/timeout/deferred errors |

#### UEFI CPER ARM Error Types (cper_arm_err_info.type)
From UEFI CPER specification, populated by firmware in GHES error status block:

| Type | Value | Description |
|------|-------|-------------|
| `CPER_ARM_CACHE_ERROR` | 0 | Cache hierarchy error |
| `CPER_ARM_TLB_ERROR` | 1 | TLB error |
| `CPER_ARM_BUS_ERROR` | 2 | Bus error |
| `CPER_ARM_VENDOR_ERROR` | 3 | Vendor-specific error |

**There are NO additional CPER ARM error types (no Tegra-specific extensions found).**

#### Who Populates What
- **SERR codes**: Hardware sets these in ERR<n>STATUS; ATF reads and logs them
- **CPER type field**: Firmware (UEFI/MM) must populate when creating CPER records
- **Tegra ATF**: Reads SERR codes but does NOT create CPER records

#### Actual RAS Errors Observed on Orin with seL4

From test logs (`/home/hlyytine/pkvm/autopilot/results/20251212-194320/sel4.log`):

**IOB Error:**
```
ERROR:   RAS Uncorrectable Error in IOB, base=0xe010000:
ERROR:   	Status = 0xe4000612
ERROR:   SERR = Error response from slave: 0x12
ERROR:   	IERR = CBB Interface Error: 0x6
ERROR:   	ADDR = 0x8000000000000000
```

**ACI Error:**
```
ERROR:   RAS Uncorrectable Error in ACI, base=0xe01a000:
ERROR:   	Status = 0xe8000904
ERROR:   SERR = Assertion failure: 0x4
ERROR:   	IERR = FillWrite Error: 0x9
ERROR:   	ADDR = 0x8000000000000000
```

**Analysis:**
- SERR 0x12 (18) = "Error response from slave" → Bus/fabric error
- SERR 0x4 = "Assertion failure" → Internal assertion
- Neither is SERR 6 or 7 (cache errors)

**ADDR Decoding** (see Section 1.3.1):
- `ADDR = 0x8000000000000000` → NS=1, PA=0x0
- **The actual physical address is ZERO** - a NULL pointer access!
- This is NOT "invalid address with bit 63 set"

**If these errors reached Linux GHES** (which they don't - see Part 11), the CPER record would contain `physical_fault_addr = 0x0`, and `pfn_valid(0)` would likely fail since PFN 0 is typically reserved.

### 8.7 Summary Table

| Scenario | Action | Process | Data |
|----------|--------|---------|------|
| Correctable + Threshold | MF_SOFT_OFFLINE | No kill | Migrate |
| Uncorrectable (Recoverable) | memory_failure | Kill (SIGBUS) | Lost |
| Fatal | - | Panic | N/A |
| PA=0x0 (NULL pointer) | Warn log | No action | Safe |
| Kernel page | MF_IGNORED | No kill | Lost |
| User dirty page | SIGBUS (force) | Forcekill | Lost |
| User clean page | Unmap only | Optional signal | Safe |

---

## Part 9: Tegra-Specific RAS Infrastructure

### 9.1 Control Backbone (CBB) Error Handlers

**Source:** `drivers/soc/tegra/cbb/tegra234-cbb.c`

Tegra234/Orin has a custom CBB fabric error handler:

```
CBB Fabric Domains:
├─ nvidia,tegra234-cbb-fabric    (main CBB)
├─ nvidia,tegra234-aon-fabric    (Always-On)
├─ nvidia,tegra234-bpmp-fabric   (BPMP controller)
├─ nvidia,tegra234-dce-fabric    (Display composition)
├─ nvidia,tegra234-rce-fabric    (Reliability engine)
└─ nvidia,tegra234-sce-fabric    (Safety engine)
```

**Error Types Supported:**
- `SLAVE_ERR` - Slave device error
- `DECODE_ERR` - Address decode error
- `FIREWALL_ERR` - Firewall violation
- `TIMEOUT_ERR` - Transaction timeout
- `PWRDOWN_ERR` - Power domain down
- `UNSUPPORTED_ERR` - Unsupported operation
- `POISON_ERR` - Poison propagation (Tegra241)

### 9.2 SError Masking

**Source:** `drivers/soc/tegra/fuse/tegra-apbmisc.c`

```c
int tegra194_miscreg_mask_serror(void)
{
    // Sets ERD (Error Response Disable) bit
    // Masks SError and converts to CBB interrupt
    writel_relaxed(ERD_MASK_INBAND_ERR, apbmisc_base + ERD_ERR_CONFIG);
}
```

This converts SError exceptions to interrupt-based reporting, preventing hard crashes.

### 9.3 ATF RAS Error Records (T234)

**Source:** ATF `plat/nvidia/tegra/soc/t234/plat_ras.c`

```
RAS Error Record Groups:
├─ CCPMU      (Core Clock/Performance Monitoring)
├─ CMU_CLOCKS (Clock Management)
├─ IH        (Interrupt Handler)
├─ IST       (Interrupt Safety Triple)
├─ IOB       (Input/Output Bridge)
├─ SNOC      (System NoC)
├─ SCC       (System Cache Controller) × 8 slices
├─ ACI       (ARM Coherency Interface) × 3 clusters
├─ CORE_DCLS (Core Data Cache) × 3 clusters
└─ GIC600_FMU (GIC Fault Management)

Total: 30+ error record groups
```

### 9.4 seL4-Specific Debug Features in ATF

The ATF RAS handler includes special seL4 debugging:

```c
// seL4 VA-to-PA translation
#define SEL4_KERNEL_VA_BASE  0x8080000000ULL
#define SEL4_KERNEL_PA_BASE  0x80000000ULL

// Context captured on RAS error:
- TTBR0_EL2 (seL4 kernel page tables)
- VTTBR_EL2 (guest page tables)
- ELR_EL3   (seL4 PC when interrupted)
- X29_EL2   (frame pointer for backtrace)
```

### 9.5 Tegra Divergences from Standard ARM RAS

1. **Cache Operations:** Uses `dc civac` instead of `dc cisw`
   - `dc cisw` doesn't work correctly on Tegra Xavier/Orin
   - See: `docs/reference/tegra-cache-operations.md`

2. **CBB Error Reporting:** Non-standard fabric error logger
   - Custom Master ID lookup tables
   - Firewall-aware error detection

3. **VTTBR_EL2 Handling:** Tegra234 RAS errors triggered by NULL VTTBR
   - Fixed in seL4 by using valid empty page table instead of NULL

### 9.6 Linux Kernel RAS Configuration (Tegra)

**Source:** `arch/arm64/configs/tegra_prod_defconfig`

```
CONFIG_ACPI_APEI=y              # APEI support
CONFIG_ACPI_APEI_GHES=y         # GHES driver
CONFIG_ACPI_APEI_MEMORY_FAILURE=y
CONFIG_ACPI_APEI_EINJ=y         # Error injection
CONFIG_EDAC_GHES=y              # EDAC integration
CONFIG_CRASH_DUMP=y             # kdump support
```

### 9.7 Gaps in Tegra Linux RAS

1. **GHES/BERT Integration:** Tegra doesn't expose platform-specific error records through standard ACPI tables

2. **CBB vs APEI:** CBB errors are reported via interrupt, not through APEI

3. **EDAC Integration:** `CONFIG_EDAC_GHES` enabled but doesn't capture CBB-specific errors

4. **seL4 RAS Context:** ATF captures seL4 state but no way to pass it back to seL4

---

## Part 10: Implications for seL4 on Orin AGX

### 10.1 Current seL4 RAS Handling

seL4 currently:
- Sets `KernelAArch64SErrorIgnore ON` - ignores SError exceptions
- Does not implement SDEI client
- Does not have GHES/ACPI error status block
- Relies on ATF to log/handle RAS errors

### 10.2 Hypothesized Root Cause of Orin RAS Errors

**Hypothesis:** seL4's TLB invalidation may have set VTTBR_EL2 to NULL (0) when switching VMIDs. Tegra234's memory subsystem potentially doesn't tolerate NULL base addresses and generates RAS errors.

**Evidence supporting this hypothesis:**
- RAS error ADDR = `0x8000000000000000` decodes to PA=0x0 (see Section 1.3.1)
- PA=0x0 is exactly what a NULL VTTBR base would produce
- Errors occur during TLB invalidation operations

**Attempted Fix:** Use `addrFromKPPtr(armKSGlobalUserVSpace)` as VTTBR base instead of NULL. This provides a valid (empty) page table that might satisfy Tegra234's speculative table walker.

**Reference:** Commit `448255694ff79fe1204c968530f6bf374670f969`

**Note:** This is still under investigation. The VTTBR_EL2=NULL hypothesis has not been definitively confirmed as the root cause. The actual cause could be:
- Speculative table walks with NULL base address causing PA=0x0 accesses
- Cache coherency issues during page table operations
- Fabric errors from other sources
- Platform-specific quirks in Tegra234's memory subsystem

### 10.3 What Linux Would Do With These Errors

**Correct ADDR interpretation**: `0x8000000000000000` means PA=0x0 (see Section 1.3.1).

If Linux received these errors (which it doesn't on Tegra - no HEST table):

1. CPER record would contain `physical_fault_addr = 0x0`
2. `ghes_do_memory_failure(0x0)` called
3. `pfn_valid(0)` returns false (PFN 0 typically reserved/invalid)
4. Warning logged: "Invalid address in generic error data: 0x0"
5. **No action taken** - error discarded, system continues

This means even Linux would just log and ignore these PA=0x0 errors, which supports the theory that NULL pointer accesses during speculative table walks are the cause.

### 10.4 Options for seL4 RAS Handling

1. **Status Quo:** Ignore SError, let ATF log errors
   - Pro: Simple, working
   - Con: No error visibility in seL4

2. **Implement Minimal SDEI Client:**
   - Register handlers for events 300-311
   - Log errors from seL4 side
   - Pro: Better visibility
   - Con: Significant implementation effort

3. **Shared Memory Error Block:**
   - Allocate memory for ATF to write error details
   - seL4 polls or gets notification
   - Pro: Rich error information
   - Con: Need ATF modifications

4. **Fix Root Causes:**
   - Already done for VTTBR_EL2 issue
   - Continue investigating other speculative access patterns
   - Pro: Eliminates errors at source
   - Con: May be platform-specific quirks

---

## Part 11: Critical Finding - Tegra Has No HEST Table

### 11.1 ACPI HEST Table Absence

**Verified on Orin AGX running Linux:**
```bash
$ cat /sys/firmware/acpi/tables/HEST
# (file does not exist)

$ dmesg | grep -i hest
# (no output)
```

**Tegra Orin does NOT provide an ACPI HEST (Hardware Error Source Table).**

### 11.2 Implications

Without HEST:
1. **No GHES driver probe:** Linux GHES driver has no sources to register
2. **No SDEI-GHES integration:** Even though ATF dispatches SDEI events, Linux has no GHES handler registered
3. **No error status blocks:** Nowhere for firmware to write CPER records
4. **No memory failure handling:** `ghes_handle_arm_hw_error()` never called

### 11.3 What Actually Happens on Tegra Linux

```
ATF RAS Error Detected
    ↓
ATF logs error to console (ERROR: RAS Uncorrectable Error...)
    ↓
ATF calls sdei_dispatch_event(300 + cpu_id)
    ↓
sdei_dispatch_event returns -1 (no handler registered)
    ↓
ATF logs: "sdei_dispatch_event returned -1"
    ↓
Error handling ends (no OS notification)
```

### 11.4 Linux RAS Statistics on Orin

Verified with tracing enabled:
```bash
$ echo 1 > /sys/kernel/debug/tracing/events/ras/enable
$ cat /sys/kernel/debug/tracing/trace
# (empty - no RAS events)

$ dmesg | grep -i "ghes\|apei\|ras\|cper"
[    0.028129] CPU features: detected: RAS Extension Support
# (no GHES or APEI messages)
```

**Result:** Linux kernel has RAS extension support, but no errors are ever reported through GHES because HEST table is missing.

### 11.5 Why This Matters for seL4

1. **No standard error delivery path:** Even if seL4 implemented SDEI client, there's no GHES infrastructure
2. **ATF-only error handling:** Tegra relies on ATF to handle RAS errors completely
3. **CBB errors separate:** Tegra CBB fabric errors use custom interrupt-based handler, not APEI

### 11.6 Comparison: Expected vs Actual Flow

**Expected (Standard ARM SDEI+GHES):**
```
Hardware Error → ATF → SDEI Event → Linux SDEI Handler → GHES Callback
    → Read CPER from error status block → Process/Log/Recover
```

**Actual (Tegra Orin):**
```
Hardware Error → ATF → Log to console → SDEI dispatch (fails) → Done
```

---

## References

- ARM SDEI Specification: ARM DEN 0054
- ARM RAS Extension: ARM DDI 0587
- ACPI APEI Specification: ACPI 6.4 Chapter 18
- Linux GHES Driver: `drivers/acpi/apei/ghes.c`
- Linux Memory Failure: `mm/memory-failure.c`
- Linux RAS Subsystem: `drivers/ras/`
- ATF SDEI Service: `services/std_svc/sdei/`
- ATF Tegra RAS: `plat/nvidia/tegra/soc/t234/plat_ras.c`
- Tegra CBB Driver: `drivers/soc/tegra/cbb/tegra234-cbb.c`
