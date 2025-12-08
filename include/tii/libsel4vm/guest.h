/*
 * Copyright 2023, Unikie
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

typedef struct guest_config {
    vm_t *vm;
    void *dtb;
    bool generate_dtb;
} guest_config_t;
