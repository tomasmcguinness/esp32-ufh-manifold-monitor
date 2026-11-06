#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define FLOW_LOOP_COUNT     3
#define FLOW_LOOP_NAME_MAX  24

typedef struct {
    char name[FLOW_LOOP_NAME_MAX];
    float scale_max_lpm;   // Top of the indicator's printed scale
    bool valid;            // false until the camera analysis produces a reading
    float flow_lpm;
    int64_t updated_us;    // esp_timer time of the last reading
} flow_loop_t;

// Load loop names from NVS (defaults "Loop 1".."Loop 3").
esp_err_t flows_init(void);

// Copy out the current state of all loops.
void flows_get(flow_loop_t out[FLOW_LOOP_COUNT]);

// Rename a loop (index 0..FLOW_LOOP_COUNT-1) and persist it.
esp_err_t flows_set_name(int index, const char *name);

// Record a reading from the analysis. Not called yet: there is no float detection.
void flows_set_reading(int index, float flow_lpm);

// Write the dashboard JSON into buf. Returns the length, or -1 if it does not fit.
int flows_build_json(char *buf, size_t len);
