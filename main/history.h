#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "flows.h"

// In-RAM history of flow readings: one sample per reading, 7 days at one a minute, in
// PSRAM. Lost on reboot.
esp_err_t history_init(void);

// Record one reading. valid[i] false stores a gap for that loop.
void history_add(const float flow_lpm[FLOW_LOOP_COUNT], const bool valid[FLOW_LOOP_COUNT]);

// Build {"nowSeconds":..,"bucketSeconds":..,"points":[[ageSeconds,l1,l2,l3],...]} for the
// last `hours`, averaged into at most 720 buckets, oldest first. Returns a malloc'd
// string the caller frees, or NULL.
char *history_build_json(int hours, size_t *out_len);
