#ifndef CONFIGD_NETWORK_H
#define CONFIGD_NETWORK_H

#include "result.h"
#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>

struct ipv4_runtime {
  char address[16];
  unsigned int prefix;
  char gateway[16];
  char broadcast[16];
  char dns[2][16];
  unsigned int mtu;
};

struct network_runtime {
  char configured_mode[8];
  char source[16];
  char state[16];
  struct ipv4_runtime applied;
  unsigned int renew_in;
  unsigned int expires_in;
  long lease_expires_at;
  long lease_acquired_at;
  long last_change;
  unsigned int consecutive_misses;
  char last_error[192];
};

struct json_object *network_default_config(void);
int network_validate_config(struct json_object *config,
                            char *error, size_t error_size);
int network_manager_init(struct json_object *config,
                         struct apply_result *result);
/* Populate runtime status from desired configuration and readable DHCP state
 * without writing any Click handlers. Intended for read-only CLI queries. */
int network_manager_observe(struct json_object *config);
int network_manager_configure(struct json_object *config,
                              struct apply_result *result,
                              bool defer_address_change);
bool network_manager_poll(struct apply_result *result);
unsigned int network_manager_next_poll_seconds(void);
struct json_object *network_manager_status_json(void);
const struct network_runtime *network_manager_runtime(void);

/* Public parser helpers used by validation and tests. */
int network_parse_cidr(const char *cidr, struct ipv4_runtime *value,
                       char *error, size_t error_size);

/* Render /etc/resolv.conf (override path with $CONFIGD_RESOLV_CONF) from the
 * IPv4 DNS servers in value. No-op when value carries no servers, and skips the
 * write when the file already matches to avoid needless NOR flash wear. Returns
 * 0 on success or when nothing needed doing, negative errno on failure. */
int network_render_resolv_conf(const struct ipv4_runtime *value);

#endif
