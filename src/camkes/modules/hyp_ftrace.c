/*
 * Copyright 2024, Unikie
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
 */

#include <autoconf.h>

#include <sel4vm/guest_vm.h>
#include <sel4vm/guest_vcpu_fault.h>
#include <utils/util.h>

#include <vmlinux.h>

#include <tii/camkes/hyp_ftrace.h>

/* Register offsets */
#define HYP_FTRACE_CMD      0x00    /* Command register (write-only) */
#define HYP_FTRACE_STATUS   0x04    /* Status register (read-only) */

/* Commands */
#define CMD_ARM   0x01    /* Clear ftrace buffer, enable recording */
#define CMD_DUMP  0x02    /* Trigger ftrace dump to UART */

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
#ifdef CONFIG_ENABLE_BENCHMARKS
                ZF_LOGI("hyp_ftrace: ARM command - resetting ftrace buffer");
                seL4_BenchmarkResetLog();
#else
                ZF_LOGW("hyp_ftrace: ARM command ignored (benchmarks disabled)");
#endif
                break;
            case CMD_DUMP:
#ifdef CONFIG_ENABLE_BENCHMARKS
                ZF_LOGI("hyp_ftrace: DUMP command - finalizing ftrace");
                seL4_BenchmarkFinalizeLog();
#else
                ZF_LOGW("hyp_ftrace: DUMP command ignored (benchmarks disabled)");
#endif
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
    ZF_LOGI("hyp_ftrace: registered at 0x%"PRIxPTR" (size 0x%zx, benchmarks enabled)",
            hf->base, hf->size);
#else
    ZF_LOGW("hyp_ftrace: registered at 0x%"PRIxPTR" but benchmarks DISABLED - commands will be no-ops",
            hf->base);
#endif
}
