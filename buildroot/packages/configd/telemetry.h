#ifndef POSTMERKOS_TELEMETRY_H
#define POSTMERKOS_TELEMETRY_H

#include "result.h"
#include "portstats.h"
#include <json-c/json.h>
#include <stddef.h>

#define TELEMETRY_PROM_DEFAULT_PORT 9100
#define TELEMETRY_MGMT_IFACE_DEFAULT "linux_mgmt"

struct json_object *telemetry_default_config(void);
int telemetry_validate(struct json_object *config, char *error, size_t error_size);

/* Apply both exporters as a required runtime part of the configuration
 * transaction.  Any listener, snapshot, environment-file, or service failure
 * is returned through result so the caller can roll back before persistence. */
int telemetry_apply(struct json_object *config, const char *bind_addr,
                    struct apply_result *result);

/* Snapshot refresh is separate from socket I/O so a slow metrics client can
 * never delay Click polling or the rest of configd's event loop. */
void telemetry_tick(void);
void telemetry_service_io(void);
void telemetry_shutdown(void);
int telemetry_interval_seconds(void);
int telemetry_server_fd(void);

int telemetry_write_snmpd_env(struct json_object *config, const char *bind_addr);

/* Host-testable enrichment seam. */
void telemetry_enrich_port_status(struct portstats_snapshot *snap);

/* Compact live-telemetry frame (temps, load, uptime, PoE, per-port raw counters,
   totals) for the WebSocket graph stream. Reads the current in-memory snapshot;
   caller owns the returned json object. */
struct json_object *telemetry_stream_json(void);

#endif
