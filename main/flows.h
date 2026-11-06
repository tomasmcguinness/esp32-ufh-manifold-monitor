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
    bool measured;         // the reader has run at least once
    float height_px;       // raw: float ring height above the cap
    float strength;        // raw: ring redness; low means the ring was not found
} flow_loop_t;

// Load loop names from NVS (defaults "Loop 1".."Loop 3").
esp_err_t flows_init(void);

// Copy out the current state of all loops.
void flows_get(flow_loop_t out[FLOW_LOOP_COUNT]);

// Rename a loop (index 0..FLOW_LOOP_COUNT-1) and persist it.
esp_err_t flows_set_name(int index, const char *name);

// Record a reading from the flow reader. `valid` false keeps the raw values for
// diagnostics but withdraws the flow figure.
void flows_set_measurement(int index, bool valid, float flow_lpm, float height_px, float strength);

// Write the dashboard JSON into buf. Returns the length, or -1 if it does not fit.
int flows_build_json(char *buf, size_t len);
