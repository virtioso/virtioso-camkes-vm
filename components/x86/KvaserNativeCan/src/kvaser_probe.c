/*
 * Copyright 2026, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <camkes.h>
#include <sel4/sel4.h>
#include <stdio.h>
#include <utils/util.h>

#include "301/CO_NMT_Heartbeat.h"
#include "canopen_runtime_service.h"
#include "kvaser_native.h"

static canopen_runtime_service_t canopen_runtime_service;

static void dump_kvaser_state(const char *tag)
{
    kvaser_native_snapshot_t snapshot;
    kvaser_native_snapshot(&snapshot);
    printf("KVASER_PROBE[%s]: bdf=%02x:%02x.%u vid=%04x did=%04x class=%04x rev=%02x cmd=%04x status=%04x line=%u pin=%u\n",
           tag, 0, 4, 0, snapshot.vendor, snapshot.device, snapshot.class_code, snapshot.revision,
           snapshot.command, snapshot.status, snapshot.int_line, snapshot.int_pin);
    printf("KVASER_PROBE[%s]: cfg bar0=%08x bar1=%08x bar2=%08x\n",
           tag, snapshot.bar0_cfg, snapshot.bar1_cfg, snapshot.bar2_cfg);
    printf("KVASER_PROBE[%s]: s5920 intcsr=%08x rcr=%08x\n", tag, snapshot.intcsr, snapshot.rcr);
    printf("KVASER_PROBE[%s]: sja mod=%02x sr=%02x ir=%02x ier=%02x btr0=%02x btr1=%02x ocr=%02x cdr=%02x xilinx=%02x\n",
           tag, snapshot.sja_mod, snapshot.sja_sr, snapshot.sja_ir, snapshot.sja_ier,
           snapshot.sja_btr0, snapshot.sja_btr1, snapshot.sja_ocr, snapshot.sja_cdr,
           snapshot.xilinx_ver);
}

static void dump_service_state(const char *tag)
{
    printf("KVASER_SERVICE[%s]: irq_count=%lu tx_req=%lu tx_done=%lu rx_irq=%lu rx_frames=%lu overruns=%lu errors=%lu last_irq=%02x\n",
           tag,
           (unsigned long)canopen_runtime_service.native_service.irq_count,
           (unsigned long)canopen_runtime_service.native_service.tx_request_count,
           (unsigned long)canopen_runtime_service.native_service.tx_complete_count,
           (unsigned long)canopen_runtime_service.native_service.rx_irq_count,
           (unsigned long)canopen_runtime_service.native_service.rx_frame_count,
           (unsigned long)canopen_runtime_service.native_service.data_overrun_count,
           (unsigned long)canopen_runtime_service.native_service.error_irq_count,
           canopen_runtime_service.native_service.last_irq_bits);
    if (canopen_runtime_service.native_service.has_last_rx) {
        printf("KVASER_SERVICE[%s]: last_rx id=%03lx dlc=%u data0=%02x extended=%u rtr=%u\n",
               tag,
               (unsigned long)canopen_runtime_service.native_service.last_rx_frame.can_id,
               canopen_runtime_service.native_service.last_rx_frame.dlc,
               canopen_runtime_service.native_service.last_rx_frame.data[0],
               canopen_runtime_service.native_service.last_rx_frame.extended ? 1U : 0U,
               canopen_runtime_service.native_service.last_rx_frame.rtr ? 1U : 0U);
    }
}

static void dump_canopen_service_state(const char *tag)
{
    CO_t *co = canopen_runtime_service.co;
    CO_NMT_internalState_t nmt_state = CO_NMT_INITIALIZING;

    if (co != NULL && co->NMT != NULL) {
        nmt_state = CO_NMT_getInternalState(co->NMT);
    }

    printf("KVASER_CANOPEN_SERVICE[%s]: started=%u node=%u bitrate=%u process_count=%lu reset=%d timer_next_us=%lu nmt=%d\n",
           tag,
           canopen_runtime_service.started ? 1U : 0U,
           canopen_runtime_service.node_id,
           canopen_runtime_service.bitrate_kbit,
           (unsigned long)canopen_runtime_service.process_count,
           (int)canopen_runtime_service.last_reset,
           (unsigned long)canopen_runtime_service.last_timer_next_us,
           (int)nmt_state);

    printf("KVASER_CANOPEN_SERVICE[%s]: port_tx=%lu port_rx=%lu native_tx=%lu native_rx=%lu heap=%lu\n",
               tag,
               (unsigned long)canopen_runtime_service.port.tx_messages,
               (unsigned long)canopen_runtime_service.port.rx_messages,
               (unsigned long)canopen_runtime_service.native_driver.tx_messages,
               (unsigned long)canopen_runtime_service.native_driver.rx_messages,
               (unsigned long)canopen_runtime_service.heap_memory_used);

    if (co != NULL && co->CANmodule != NULL) {
        printf("KVASER_CANOPEN_SERVICE[%s]: can_normal=%u tx_count=%u can_error=%04x has_last_rx=%u\n",
               tag,
               co->CANmodule->CANnormal ? 1U : 0U,
               (unsigned)co->CANmodule->CANtxCount,
               (unsigned)co->CANmodule->CANerrorStatus,
               co->CANmodule->has_last_rx_msg ? 1U : 0U);
        if (co->CANmodule->has_last_rx_msg) {
            printf("KVASER_CANOPEN_SERVICE[%s]: last_rx ident=%03x dlc=%u data0=%02x\n",
                   tag,
                   CO_CANrxMsg_readIdent(&co->CANmodule->last_rx_msg),
                   CO_CANrxMsg_readDLC(&co->CANmodule->last_rx_msg),
                   CO_CANrxMsg_readData(&co->CANmodule->last_rx_msg)[0]);
        }
    }
}

static void dump_canopen_start_failure(void)
{
    printf("KVASER_CANOPEN_SERVICE[init-failed]: stage=%d err=%d err_info=%08lx heap=%lu started=%u\n",
           canopen_runtime_service.last_start_stage,
           canopen_runtime_service.last_start_error,
           (unsigned long)canopen_runtime_service.last_err_info,
           (unsigned long)canopen_runtime_service.heap_memory_used,
           canopen_runtime_service.started ? 1U : 0U);
}

static void dump_runtime_state(const char *tag)
{
    const canopen_runtime_state_t *state =
        canopen_runtime_service_state(&canopen_runtime_service);

    if (state == NULL) {
        return;
    }

    printf("CANOPEN_RUNTIME[%s]: seq=%lu cycle=%lu uptime_ms=%lu node=%u nmt=%u started=%u can_normal=%u has_last_rx=%u status=%04x timer_next_us=%lu\n",
           tag,
           (unsigned long)state->sequence,
           (unsigned long)state->cycle_count,
           (unsigned long)state->uptime_ms,
           state->node_id,
           state->nmt_state,
           state->started ? 1U : 0U,
           state->can_normal ? 1U : 0U,
           state->has_last_rx ? 1U : 0U,
           state->status_word,
           (unsigned long)state->last_timer_next_us);
    printf("CANOPEN_RUNTIME[%s]: can_diag valid=%u tx=%lu rx=%lu boff=%lu ovr=%lu\n",
           tag,
           state->can_diag_valid ? 1U : 0U,
           (unsigned long)state->can_tx_err_count,
           (unsigned long)state->can_rx_err_count,
           (unsigned long)state->can_bus_off_count,
           (unsigned long)state->can_overrun_count);
}

void pre_init(void)
{
    set_putchar(putchar_putchar);
    canopen_runtime_service_init(&canopen_runtime_service);
    printf("KVASER_PROBE: pre_init\n");
}

int run(void)
{
    dump_kvaser_state("boot");
    printf("KVASER_PROBE: canopen service start\n");
    if (!canopen_runtime_service_start(&canopen_runtime_service, 10, 125)) {
        dump_canopen_start_failure();
        dump_kvaser_state("init-failed");
        while (1) {
            seL4_Yield();
        }
    }
    dump_kvaser_state("post-init");
    dump_service_state("post-init");
    dump_canopen_service_state("post-init");
    dump_runtime_state("post-init");
    printf("KVASER_PROBE: request initial sync from real CANopen service\n");
    if (!canopen_runtime_service_send_sync(&canopen_runtime_service)) {
        printf("KVASER_PROBE: failed to queue sync frame\n");
    }
    dump_kvaser_state("post-tx");
    dump_service_state("post-tx");
    dump_runtime_state("post-tx");
    while (1) {
        canopen_runtime_service_process(&canopen_runtime_service, 1000);
        if ((canopen_runtime_service.process_count % 1000U) == 0U) {
            dump_canopen_service_state("loop");
            dump_runtime_state("loop");
        }
        seL4_Yield();
    }
    UNREACHABLE();
}

void irq_handle(void)
{
    canopen_runtime_service_handle_irq(&canopen_runtime_service);
    dump_kvaser_state("irq");
    dump_service_state("irq");
    dump_canopen_service_state("irq");
    dump_runtime_state("irq");
    int err = irq_acknowledge();
    ZF_LOGF_IF(err, "Failed to acknowledge Kvaser IRQ");
}
