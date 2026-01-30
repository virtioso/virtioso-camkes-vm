/*
 * Copyright 2024, Unikie
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * CAmkES template for hypervisor ftrace control interface.
 * Enabled by setting hyp_ftrace configuration attribute to the MMIO base address.
 */

#include <camkes.h>
#include <vmlinux.h>
#include <utils/util.h>

#include <tii/camkes/hyp_ftrace.h>

/*- set hyp_ftrace = configuration[me.name].get('hyp_ftrace') -*/
/*- if hyp_ftrace is not none -*/
static hyp_ftrace_t hyp_ftrace = {
    .base = /*? hyp_ftrace ?*/,
    .size = BIT(PAGE_BITS_4K),
};

DEFINE_MODULE(hyp_ftrace, &hyp_ftrace, hyp_ftrace_init)
/*- endif -*/
