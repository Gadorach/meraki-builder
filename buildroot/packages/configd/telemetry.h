#ifndef POSTMERKOS_TELEMETRY_H
#define POSTMERKOS_TELEMETRY_H

#include <json-c/json.h>

/* Default Prometheus listen port (spec §3.3). */
#define TELEMETRY_PROM_DEFAULT_PORT 9100

/* Build the default telemetry config object (both exporters disabled). */
struct json_object *telemetry_default_config(void);

/* Validate the telemetry section of a config object.
 * Returns 0 if absent or valid; -EINVAL with error filled otherwise. */
int telemetry_validate(struct json_object *config, char *error, size_t error_size);

/* Apply telemetry config: (re)start or stop the Prometheus server to match.
 * bind_addr is the current management IPv4 (may be NULL/empty for ANY). */
void telemetry_apply(struct json_object *config, const char *bind_addr);

/* Called on the main-loop tick: refresh the shared snapshot from Click + status,
 * render the SNMP stats file, and (if running) service the HTTP server. */
void telemetry_tick(void);

/* The poll interval (seconds) for snapshot refresh. */
int telemetry_interval_seconds(void);

/* Listening fd for the metrics server, or -1. Lets main.c add it to a poll set
 * (optional optimization; telemetry_tick() also services it). */
int telemetry_server_fd(void);

/* Render /run/postmerkos/snmpd.env (or $CONFIGD_SNMPD_ENV) from telemetry.snmp.
 * bind_addr is the management IPv4 (may be NULL/empty). Returns 0 / -1. */
int telemetry_write_snmpd_env(struct json_object *config, const char *bind_addr);

#endif
