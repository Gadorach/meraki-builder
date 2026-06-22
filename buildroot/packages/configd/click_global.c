#include "click_global.h"
#include "configd.h"

#include <libpostmerkos.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct json_object *read_stp(void) {
  struct json_object *stp = json_object_new_object();
  json_object_object_add(stp, "priority", json_object_new_int(32768));
  json_object_object_add(stp, "hello_time", json_object_new_int(2));
  json_object_object_add(stp, "forward_delay", json_object_new_int(15));
  json_object_object_add(stp, "max_age", json_object_new_int(20));
  json_object_object_add(stp, "hold_count", json_object_new_int(6));
  return stp;
}

static struct json_object *read_lacp(struct apply_result *result) {
  struct json_object *lacp = json_object_new_object();
  char buffer[32];
  int rc = click_read("/click/switch_port_table/enable_lacp_on_single_ports",
                      buffer, sizeof(buffer));
  if (rc != 0) {
    apply_result_warn(result, "LACP state unavailable; default used");
    snprintf(buffer, sizeof(buffer), "true");
  }
  json_object_object_add(lacp, "enabled",
      json_object_new_boolean(!strcmp(buffer, "true") || !strcmp(buffer, "1")));
  return lacp;
}

static struct json_object *read_multicast(void) {
  struct json_object *multicast = json_object_new_object();
  json_object_object_add(multicast, "igmp_snooping", json_object_new_boolean(true));
  json_object_object_add(multicast, "igmp_querier_interval", json_object_new_int(125));
  json_object_object_add(multicast, "mld_snooping", json_object_new_boolean(true));
  json_object_object_add(multicast, "mld_querier_interval", json_object_new_int(125));
  return multicast;
}

struct json_object *click_read_globals(struct apply_result *result) {
  struct json_object *globals = json_object_new_object();
  json_object_object_add(globals, "stp", read_stp());
  json_object_object_add(globals, "lacp", read_lacp(result));
  json_object_object_add(globals, "multicast", read_multicast());
  return globals;
}

static int checked_write(const char *field, const char *path,
                         const char *value, bool required,
                         struct apply_result *result) {
  printf("%s global field=%s value=%s%s\n", get_time(), field, value,
         dry_run ? " dry_run=true" : "");
  int rc = dry_run ? 0 : click_write(path, value);
  if (rc != 0) {
    if (required) apply_result_fail(result, "global %s failed: %s", field, strerror(-rc));
    else apply_result_warn(result, "optional global %s failed: %s", field, strerror(-rc));
  } else
    apply_result_applied(result);
  return rc;
}

static int apply_stp(struct json_object *value, struct apply_result *result) {
  char command[256];
  snprintf(command, sizeof(command),
      "PRIORITY %d, HELLO_TIME %d, FORWARD_DELAY %d, MAX_AGE %d, HOLDCOUNT %d",
      json_object_get_int(json_object_object_get(value, "priority")),
      json_object_get_int(json_object_object_get(value, "hello_time")),
      json_object_get_int(json_object_object_get(value, "forward_delay")),
      json_object_get_int(json_object_object_get(value, "max_age")),
      json_object_get_int(json_object_object_get(value, "hold_count")));
  return checked_write("stp", "/click/stp/set_params", command, true, result);
}

static int apply_lacp(struct json_object *value, struct apply_result *result) {
  bool enabled = json_object_get_boolean(json_object_object_get(value, "enabled"));
  return checked_write("lacp", "/click/switch_port_table/enable_lacp_on_single_ports",
                       enabled ? "true" : "false", true, result);
}

static int apply_multicast(struct json_object *value,
                           struct apply_result *result) {
  bool igmp = json_object_get_boolean(
      json_object_object_get(value, "igmp_snooping"));
  bool mld = json_object_get_boolean(
      json_object_object_get(value, "mld_snooping"));
  int igmp_interval = json_object_get_int(
      json_object_object_get(value, "igmp_querier_interval"));
  int mld_interval = json_object_get_int(
      json_object_object_get(value, "mld_querier_interval"));
  int rc = 0;
  char buffer[32];

  if (checked_write("multicast.igmp_snooping",
                    "/click/configure_igmp_snoop/run",
                    igmp ? "true" : "false", false, result) != 0) rc = -EIO;
  snprintf(buffer, sizeof(buffer), "%d", igmp_interval * 1000);
  if (checked_write("multicast.igmp_querier_interval",
                    "/click/igmp_snoop/default_querier_interval_msec",
                    buffer, false, result) != 0) rc = -EIO;
  snprintf(buffer, sizeof(buffer), "%d", igmp_interval);
  if (checked_write("multicast.igmp_query_interval",
                    "/click/igmp_querier/query_interval",
                    buffer, false, result) != 0) rc = -EIO;

  if (checked_write("multicast.mld_snooping",
                    "/click/configure_mld_snoop/run",
                    mld ? "true" : "false", false, result) != 0) rc = -EIO;
  snprintf(buffer, sizeof(buffer), "%d", mld_interval * 1000);
  if (checked_write("multicast.mld_querier_interval",
                    "/click/mld_snoop/default_querier_interval_msec",
                    buffer, false, result) != 0) rc = -EIO;
  snprintf(buffer, sizeof(buffer), "%d", mld_interval);
  if (checked_write("multicast.mld_query_interval",
                    "/click/mld_querier/query_interval",
                    buffer, false, result) != 0) rc = -EIO;
  return rc;
}

static int apply_globals(struct json_object *full_config,
                         struct json_object *delta,
                         struct apply_result *result) {
  struct json_object *value = NULL;
  struct json_object *changed = NULL;
  int rc = 0;
  if ((!delta || json_object_object_get_ex(delta, "stp", &changed)) &&
      json_object_object_get_ex(full_config, "stp", &value) &&
      apply_stp(value, result) != 0) rc = -EIO;
  if ((!delta || json_object_object_get_ex(delta, "lacp", &changed)) &&
      json_object_object_get_ex(full_config, "lacp", &value) &&
      apply_lacp(value, result) != 0) rc = -EIO;
  if ((!delta || json_object_object_get_ex(delta, "multicast", &changed)) &&
      json_object_object_get_ex(full_config, "multicast", &value))
    apply_multicast(value, result); /* donor-dependent handlers are optional */
  return rc;
}

int click_apply_globals_full(struct json_object *config,
                             struct apply_result *result) {
  return apply_globals(config, NULL, result);
}

int click_apply_globals_delta(struct json_object *full_config,
                              struct json_object *delta,
                              struct apply_result *result) {
  return apply_globals(full_config, delta, result);
}
