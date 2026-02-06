/*
 * Copyright 2024, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hypervisor Ftrace Control Interface
 *
 * Memory-mapped device that allows guest Linux to control seL4 kernel ftrace
 * via benchmark syscalls. Used for debugging guest-triggered kernel activity.
 *
 * Note: Requires CONFIG_ENABLE_BENCHMARKS to be enabled in the kernel config.
 * When benchmarks are disabled, the module still initializes but commands
 * become no-ops with a warning message.
 *
 * Auto-dump feature: If armed and no vGIC interrupts are injected for 5 seconds,
 * automatically triggers ftrace dump. This helps capture traces when the guest
 * hangs before generating any interrupts.
 */

#include <autoconf.h>

#include <sel4vm/guest_vm.h>
#include <sel4vm/guest_vcpu_fault.h>
#include <sel4vm/guest_irq_controller.h>
#include <utils/util.h>

#include <vmlinux.h>

#include <virtioso/camkes/hyp_ftrace.h>

/* Register offsets */
#define HYP_FTRACE_CMD      0x00    /* Command register (write-only) */
#define HYP_FTRACE_STATUS   0x04    /* Status register (read-only) */

/* Commands */
#define CMD_ARM   0x01    /* Clear ftrace buffer, enable recording */
#define CMD_DUMP  0x02    /* Trigger ftrace dump to UART */

/* Auto-dump timeout: 5 seconds worth of 1-second timer ticks */
#define HYP_FTRACE_TIMEOUT_TICKS  5

/* State for auto-dump feature */
static bool hyp_ftrace_armed = false;
static unsigned int hyp_ftrace_ticks_since_irq = 0;

/* Called when ARM command received */
static void hyp_ftrace_do_arm(void)
{
#ifdef CONFIG_ENABLE_BENCHMARKS
    ZF_LOGE("hyp_ftrace: ARM command - resetting ftrace buffer");
    seL4_BenchmarkResetLog();
    hyp_ftrace_armed = true;
    hyp_ftrace_ticks_since_irq = 0;
#else
    ZF_LOGE("hyp_ftrace: ARM command ignored (benchmarks disabled)");
#endif
}

/* Called when DUMP command received */
static void hyp_ftrace_do_dump(void)
{
#ifdef CONFIG_ENABLE_BENCHMARKS
    ZF_LOGE("hyp_ftrace: DUMP command - finalizing ftrace");
    seL4_BenchmarkFinalizeLog();
    hyp_ftrace_armed = false;
#else
    ZF_LOGE("hyp_ftrace: DUMP command ignored (benchmarks disabled)");
#endif
}

/* Public: notify that an IRQ was injected */
void hyp_ftrace_irq_injected(void)
{
    if (hyp_ftrace_armed) {
        hyp_ftrace_ticks_since_irq = 0;
    }
}

/* Public: handle timer notification (called every 1 second) */
void hyp_ftrace_handle_timer_notification(void)
{
#ifdef CONFIG_ENABLE_BENCHMARKS
    if (!hyp_ftrace_armed) {
        return;
    }

    hyp_ftrace_ticks_since_irq++;

    if (hyp_ftrace_ticks_since_irq >= HYP_FTRACE_TIMEOUT_TICKS) {
        ZF_LOGE("hyp_ftrace: AUTO-DUMP - no IRQs for %d seconds",
                HYP_FTRACE_TIMEOUT_TICKS);
        seL4_BenchmarkFinalizeLog();
        hyp_ftrace_armed = false;
    }
#endif
}

static memory_fault_result_t hyp_ftrace_fault_handler(vm_t *vm, vm_vcpu_t *vcpu,
                                                       uintptr_t paddr, size_t len,
                                                       void *cookie)
{
    hyp_ftrace_t *hf = cookie;
    uintptr_t offset = paddr - hf->base;

    if (is_vcpu_read_fault(vcpu)) {
        switch (offset) {
        case HYP_FTRACE_STATUS:
            /* Always return idle (0) - operations are synchronous */
            set_vcpu_fault_data(vcpu, 0);
            break;
        default:
            ZF_LOGW("hyp_ftrace: unhandled read at offset 0x%lx", offset);
            return FAULT_UNHANDLED;
        }
    } else {
        seL4_Word value = emulate_vcpu_fault(vcpu, 0);
        switch (offset) {
        case HYP_FTRACE_CMD:
            switch (value) {
            case CMD_ARM:
                hyp_ftrace_do_arm();
                break;
            case CMD_DUMP:
                hyp_ftrace_do_dump();
                break;
            default:
                ZF_LOGW("hyp_ftrace: unknown command 0x%lx", value);
            }
            break;
        default:
            ZF_LOGW("hyp_ftrace: unhandled write at offset 0x%lx", offset);
            return FAULT_UNHANDLED;
        }
    }

    advance_vcpu_fault(vcpu);
    return FAULT_HANDLED;
}

void hyp_ftrace_init(vm_t *vm, void *cookie)
{
    hyp_ftrace_t *hf = cookie;
    vm_memory_reservation_t *res;

    res = vm_reserve_memory_at(vm, hf->base, hf->size,
                               hyp_ftrace_fault_handler, cookie);
    ZF_LOGF_IF(!res, "hyp_ftrace: cannot reserve memory at 0x%"PRIxPTR, hf->base);
#ifdef CONFIG_ENABLE_BENCHMARKS
    ZF_LOGE("hyp_ftrace: registered at 0x%"PRIxPTR" (size 0x%zx, benchmarks enabled)",
            hf->base, hf->size);
#else
    ZF_LOGE("hyp_ftrace: registered at 0x%"PRIxPTR" but benchmarks DISABLED - commands will be no-ops",
            hf->base);
#endif
}
