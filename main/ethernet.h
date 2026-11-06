#pragma once

#include "esp_err.h"
#include <stdbool.h>

// Bring up the W5500 and start DHCP. Returns once the driver is started; the link
// and the IP address arrive asynchronously.
esp_err_t ethernet_start(void);

// Block until an IPv4 address is assigned, or the timeout expires.
bool ethernet_wait_for_ip(int timeout_ms);

// Dotted-quad of the current IPv4 address, or "0.0.0.0".
const char *ethernet_ip_string(void);
