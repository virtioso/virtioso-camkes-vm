/*
 * Copyright 2024, Technology Innovation Institute
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * CAmkES template for hypervisor ftrace control interface.
 * Enabled by setting hyp_ftrace configuration attribute to the MMIO base address.
 *
 * Optional timer support: If hyp_ftrace_timer interface is connected (via
 * TimeServer), enables auto-dump feature that triggers ftrace dump if no
 * vGIC interrupts are injected for 5 seconds after ARM command.
 */

#include <camkes.h>
#include <vmlinux.h>
#include <utils/util.h>

#include <virtioso/camkes/hyp_ftrace.h>

/*- set hyp_ftrace = configuration[me.name].get('hyp_ftrace') -*/
/*- if hyp_ftrace is not none -*/
static hyp_ftrace_t hyp_ftrace = {
    .base = /*? hyp_ftrace ?*/,
    .size = BIT(PAGE_BITS_4K),
};

/*
 * Timer functions are provided by CAmkES when hyp_ftrace_timer interface
 * is connected. Use weak symbols so we can check at runtime if they exist.
 * Note: seL4TimeServer connector provides notification via _notification_badge(),
 * not a separate GlobalAsynch connection.
 */
extern int hyp_ftrace_timer_periodic(int tid, uint64_t ns) WEAK;
extern unsigned int hyp_ftrace_timer_completed(void) WEAK;
extern seL4_Word hyp_ftrace_timer_notification_badge(void) WEAK;

static int hyp_ftrace_timer_handler(vm_t *vm, void *cookie)
{
    /* Acknowledge timer */
    if (hyp_ftrace_timer_completed) {
        hyp_ftrace_timer_completed();
    }

    /* Check timeout and auto-dump if needed */
    hyp_ftrace_handle_timer_notification();

    return 0;
}

static void hyp_ftrace_module_init(vm_t *vm, void *cookie)
{
    hyp_ftrace_init(vm, cookie);

    /* Register periodic timer for auto-dump timeout (1 second) if available */
    if (hyp_ftrace_timer_periodic && hyp_ftrace_timer_notification_badge) {
        seL4_Word badge = hyp_ftrace_timer_notification_badge();

        int err = register_async_event_handler(badge, hyp_ftrace_timer_handler, NULL);
        if (err) {
            ZF_LOGW("hyp_ftrace: failed to register timer handler, auto-dump disabled");
            return;
        }

        /* Start 1-second periodic timer (timer ID 0, 1 billion ns = 1 second) */
        err = hyp_ftrace_timer_periodic(0, 1000000000ULL);
        if (err) {
            ZF_LOGW("hyp_ftrace: failed to start periodic timer, auto-dump disabled");
            return;
        }

        ZF_LOGE("hyp_ftrace: auto-dump timer registered (5 second timeout)");
    } else {
        ZF_LOGE("hyp_ftrace: timer not connected, auto-dump disabled");
    }
}

DEFINE_MODULE(hyp_ftrace, &hyp_ftrace, hyp_ftrace_module_init)
/*- endif -*/
