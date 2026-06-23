#ifndef POSTMERKOS_METRICS_H
#define POSTMERKOS_METRICS_H

#include <stddef.h>
#include "portstats.h"

#define METRICS_MAX_TEMPS 8

struct device_health {
  long uptime_seconds;
  int temp_count;
  double temps_celsius[METRICS_MAX_TEMPS];
  char temp_labels[METRICS_MAX_TEMPS][32];  /* e.g. "thermal_zone0", "poe0" */
  int poe_available;
  double poe_power_watts;   /* aggregate; valid iff poe_available */
  double poe_budget_watts;  /* aggregate; valid iff poe_available */
};

/*
 * Render Prometheus exposition into buf (capacity n).
 * Returns bytes written (excluding NUL) on success, -1 if buf is too small
 * (in which case buf content is undefined and MUST NOT be served).
 * snap may be NULL or have valid==0; in that case scrape_success is 0 and no
 * per-port series are emitted, but the output is still valid exposition text.
 */
int metrics_render(char *buf, size_t n,
                   const struct portstats_snapshot *snap,
                   const struct device_health *health);

/* HTTP server (implemented in Task 5). */
int metrics_server_start(const char *bind_addr, int port);
void metrics_server_stop(void);
int metrics_server_running(void);
int metrics_server_fd(void);   /* listening fd for the main-loop poll set, or -1 */

/* Service one accept/serve cycle on the listening fd (non-blocking-ish).
 * Renders from the snapshot+health provided by the caller. */
void metrics_server_service(const struct portstats_snapshot *snap,
                            const struct device_health *health);

#endif
