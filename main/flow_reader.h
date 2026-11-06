#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Periodically capture a full-resolution frame, find each tube's float ring and publish
// the flow through flows_set_measurement(). Same algorithm as tools/flow_prototype.py.
esp_err_t flow_reader_start(void);

// Take a reading now instead of waiting for the next interval. Non-blocking.
void flow_reader_trigger(void);

// Calibration from the most recent reading:
//   flow_reader_calibrate_zero(): every loop is at 0 L/min right now.
//   flow_reader_calibrate_flow(lpm): every loop reads `lpm` right now (needs a zero first).
// loop is 0..FLOW_LOOP_COUNT-1, or -1 for all loops.
esp_err_t flow_reader_calibrate_zero(int loop);
esp_err_t flow_reader_calibrate_flow(int loop, float lpm);
