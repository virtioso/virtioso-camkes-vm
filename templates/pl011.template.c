/*
 * Copyright 2024, Unikie
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <camkes.h>
#include <vmlinux.h>
#include <utils/util.h>
#include <libfdt.h>

#include <virtioso/camkes/pl011.h>
#include <virtioso/fdt.h>
/*- set pl011 = configuration[me.name].get('pl011') -*/
/*- set pl011_dtb_node = configuration[me.name].get('pl011_dtb_node') -*/
/*- set pl011_irq_spi = configuration[me.name].get('pl011_irq_spi', 1) -*/
/*- if pl011 is not none -*/
static pl011_t pl011 = {
    .base = /*? pl011 ?*/,
    .size = BIT(PAGE_BITS_4K),
    .irq = 32 + /*? pl011_irq_spi ?*/,
};

DEFINE_MODULE(pl011, &pl011, pl011_init)

/*- if pl011_dtb_node -*/
static int fdt_node_generate_pl011(fdt_node_t *node, void *fdt)
{
    int err;
    int root = fdt_path_offset(fdt, "/");
    if (root < 0) {
        return root;
    }

    int clock = fdt_path_offset(fdt, "/apb-pclk");
    if (clock < 0) {
        clock = fdt_add_subnode(fdt, root, "apb-pclk");
        if (clock < 0) {
            return clock;
        }
        err = fdt_setprop_string(fdt, clock, "compatible", "fixed-clock");
        if (err) {
            return err;
        }
        err = fdt_setprop_u32(fdt, clock, "#clock-cells", 0);
        if (err) {
            return err;
        }
        err = fdt_setprop_u32(fdt, clock, "clock-frequency", 24000000);
        if (err) {
            return err;
        }
        err = fdt_setprop_string(fdt, clock, "clock-output-names", "clk24mhz");
        if (err) {
            return err;
        }
    }

    uint32_t clock_phandle = fdt_get_phandle(fdt, clock);
    if (!clock_phandle) {
        clock_phandle = fdt_get_max_phandle(fdt) + 1;
        err = fdt_setprop_u32(fdt, clock, "phandle", clock_phandle);
        if (err) {
            return err;
        }
    }

    char name[32];
    int n = snprintf(name, sizeof(name), "pl011@%"PRIxPTR, pl011.base);
    if (n < 0 || n >= (int)sizeof(name)) {
        return -FDT_ERR_INTERNAL;
    }

    int off = fdt_add_subnode(fdt, root, name);
    if (off == -FDT_ERR_EXISTS) {
        return 0;
    }
    if (off < 0) {
        return off;
    }

    const char compatible[] = "arm,pl011\0arm,primecell";
    err = fdt_setprop(fdt, off, "compatible", compatible, sizeof(compatible));
    if (err) {
        return err;
    }

    uint64_t reg[] = {
        cpu_to_fdt64(pl011.base),
        cpu_to_fdt64(pl011.size),
    };
    err = fdt_setprop(fdt, off, "reg", reg, sizeof(reg));
    if (err) {
        return err;
    }

    uint32_t interrupts[] = {
        cpu_to_fdt32(0),
        cpu_to_fdt32(/*? pl011_irq_spi ?*/),
        cpu_to_fdt32(4),
    };
    err = fdt_setprop(fdt, off, "interrupts", interrupts, sizeof(interrupts));
    if (err) {
        return err;
    }

    uint32_t clocks[] = {
        cpu_to_fdt32(clock_phandle),
        cpu_to_fdt32(clock_phandle),
    };
    err = fdt_setprop(fdt, off, "clocks", clocks, sizeof(clocks));
    if (err) {
        return err;
    }

    const char clock_names[] = "uartclk\0apb_pclk";
    err = fdt_setprop(fdt, off, "clock-names", clock_names, sizeof(clock_names));
    if (err) {
        return err;
    }

    node->generated = true;
    return 1;
}

static fdt_node_t fdt_pl011 = {
    .name = "pl011",
    .compatible = "arm,pl011",
    .generate = fdt_node_generate_pl011,
};

DEFINE_FDT_NODE(fdt_pl011, &fdt_pl011)
/*- endif -*/
/*- endif -*/
