#include "click_port.h"
#include "configd.h"
#include "json_util.h"

#include <libpostmerkos.h>
#include <libpd690xx.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct speed_map {
  const char *json;
  const char *click;
};

static const struct speed_map speed_table[] = {
  {"auto", "aneg"}, {"10half", "10hdx"}, {"10full", "10fdx"},
  {"100half", "100hdx"}, {"100full", "100fdx"},
  {"1000full", "1000fdx"},
};

static void log_change(unsigned int port, const char *field,
                       const char *value) {
  printf("%s port=%u field=%s value=%s%s\n", get_time(), port, field,
         value ? value : "", dry_run ? " dry_run=true" : "");
}

static const char *speed_click_to_json(const char *click_mode) {
  for (size_t i = 0; i < sizeof(speed_table) / sizeof(speed_table[0]); i++)
    if (!strcmp(speed_table[i].click, click_mode)) return speed_table[i].json;
  return "auto";
}

static const char *speed_json_to_click(const char *json_speed) {
  for (size_t i = 0; i < sizeof(speed_table) / sizeof(speed_table[0]); i++)
    if (!strcmp(speed_table[i].json, json_speed)) return speed_table[i].click;
  return "aneg";
}

static struct json_object *read_phy(unsigned int port,
                                    struct apply_result *result) {
  struct json_object *phy = json_object_new_object();
  char line[512];
  char mode[32];
  int rc = read_switch_port_table("dump_port_phy_cfgs", port,
                                  line, sizeof(line));
  if (rc != 0 || get_field_copy(line, 2, mode, sizeof(mode)) != 0) {
    apply_result_warn(result, "port %u PHY state unavailable; defaults used", port);
    snprintf(mode, sizeof(mode), "aneg");
  }
  bool enabled = strcmp(mode, "off") != 0;
  json_object_object_add(phy, "enabled", json_object_new_boolean(enabled));
  json_object_object_add(phy, "speed",
      json_object_new_string(enabled ? speed_click_to_json(mode) : "auto"));
  /* These values are write-only in the available binary Click graph. */
  json_object_object_add(phy, "flow_control", json_object_new_boolean(false));
  json_object_object_add(phy, "eee", json_object_new_boolean(true));
  return phy;
}

static struct json_object *read_vlan(unsigned int port,
                                     struct apply_result *result) {
  struct json_object *vlan = json_object_new_object();
  char line[512];
  char tagged[32] = "0";
  char pvid[32] = "1";
  char untagged_vid[32] = "1";
  char allowed[256] = "";
  int rc = read_switch_port_table("dump_pport_vlans", port,
                                  line, sizeof(line));
  if (rc == 0) {
    get_field_copy(line, 2, tagged, sizeof(tagged));
    get_field_copy(line, 6, pvid, sizeof(pvid));
    get_field_copy(line, 7, untagged_vid, sizeof(untagged_vid));
    get_field_copy(line, 11, allowed, sizeof(allowed));
  } else {
    apply_result_warn(result, "port %u VLAN state unavailable; defaults used", port);
  }

  bool is_tagged = !strcmp(tagged, "1") || !strcmp(tagged, "true");
  int pvid_number = atoi(pvid);
  int untagged_number = atoi(untagged_vid);
  const char *mode = is_tagged ?
      (untagged_number > 0 ? "hybrid" : "trunk") : "access";
  json_object_object_add(vlan, "mode", json_object_new_string(mode));
  json_object_object_add(vlan, "pvid",
      json_object_new_int(pvid_number > 0 ? pvid_number : 1));
  json_object_object_add(vlan, "allowed",
      json_object_new_string(is_tagged ? allowed : ""));
  json_object_object_add(vlan, "untagged_vid",
      json_object_new_int(untagged_number >= 0 ? untagged_number : 1));
  json_object_object_add(vlan, "ingress_filter", json_object_new_boolean(true));
  return vlan;
}

static struct json_object *default_stp(void) {
  struct json_object *stp = json_object_new_object();
  json_object_object_add(stp, "enabled", json_object_new_boolean(true));
  json_object_object_add(stp, "priority", json_object_new_int(128));
  json_object_object_add(stp, "cost", json_object_new_int(0));
  json_object_object_add(stp, "edge", json_object_new_boolean(false));
  json_object_object_add(stp, "auto_edge", json_object_new_boolean(true));
  return stp;
}

static struct json_object *read_poe(unsigned int port,
                                    struct apply_result *result) {
  struct json_object *poe = json_object_new_object();
  bool enabled = false;
  const char *mode = "af";
  if (hardware.poe_available) {
    int state = port_state(&pd690xx, (int)port);
    int type = port_type(&pd690xx, (int)port);
    if (state >= 0) enabled = state != PORT_DISABLED;
    else apply_result_warn(result, "port %u PoE state unavailable; default used", port);
    if (type == PORT_MODE_AF) mode = "af";
    else if (type == PORT_MODE_AT) mode = "at";
    else apply_result_warn(result, "port %u PoE mode unavailable; default used", port);
  } else {
    apply_result_warn(result,
        "PoE controllers are unavailable; desired defaults retained for port %u",
        port);
  }
  json_object_object_add(poe, "enabled", json_object_new_boolean(enabled));
  json_object_object_add(poe, "mode", json_object_new_string(mode));
  json_object_object_add(poe, "policy", json_object_new_string("normal"));
  json_object_object_add(poe, "observation_seconds", json_object_new_int(300));
  return poe;
}

struct json_object *click_read_ports(struct apply_result *result) {
  struct json_object *root = json_object_new_object();
  struct json_object *ports = json_object_new_object();
  json_object_object_add(root, "ports", ports);

  unsigned int count = hardware.port_count;
  if (!count) {
    FILE *file = fopen(PORTS_FILE, "r");
    if (file) {
      char line[512];
      bool first = true;
      while (fgets(line, sizeof(line), file)) {
        if (first) first = false;
        else count++;
      }
      fclose(file);
    }
  }

  for (unsigned int port = 1; port <= count; port++) {
    char key[16];
    snprintf(key, sizeof(key), "%u", port);
    struct json_object *port_config = read_phy(port, result);
    json_object_object_add(port_config, "name", json_object_new_string(""));
    /* Storm control has a setter but no readable handler in the binary
     * SwitchPortTable. It is persistent desired state and is replayed at boot. */
    json_object_object_add(port_config, "storm_control",
                           json_object_new_boolean(true));
    json_object_object_add(port_config, "vlan", read_vlan(port, result));
    json_object_object_add(port_config, "stp", default_stp());
    if (hardware_port_supports_poe(&hardware, port))
      json_object_object_add(port_config, "poe", read_poe(port, result));
    json_object_object_add(ports, key, port_config);
  }
  return root;
}

static int write_handler(unsigned int port, const char *handler,
                         const char *field, const char *command, bool required,
                         struct apply_result *result) {
  log_change(port, field, command);
  if (dry_run) {
    apply_result_applied(result);
    return 0;
  }
  int rc = write_switch_port_table(handler, command);
  if (rc != 0) {
    if (required)
      apply_result_fail(result, "port %u %s failed: %s", port, field,
                        strerror(-rc));
    else
      apply_result_warn(result, "port %u %s unavailable: %s", port, field,
                        strerror(-rc));
    return required ? rc : 0;
  }
  apply_result_applied(result);
  return 0;
}

static int apply_phy(unsigned int port, struct json_object *port_config,
                     struct apply_result *result) {
  bool enabled = json_object_get_boolean(
      json_object_object_get(port_config, "enabled"));
  const char *speed = json_object_get_string(
      json_object_object_get(port_config, "speed"));
  bool flow_control = json_object_get_boolean(
      json_object_object_get(port_config, "flow_control"));
  bool eee = json_object_get_boolean(json_object_object_get(port_config, "eee"));
  const char *mode = enabled ? speed_json_to_click(speed) : "off";
  char command[160];
  snprintf(command, sizeof(command),
           "PORT %u, FC_OBEY %s, EEE_ADV_ENABLED %s, MODE %s",
           port, flow_control ? "true" : "false",
           eee ? "true" : "false", mode);
  return write_handler(port, "set_port_phy_cfgs", "phy", command, true, result);
}

static int apply_storm(unsigned int port, struct json_object *port_config,
                       struct apply_result *result) {
  bool enabled = json_object_get_boolean(
      json_object_object_get(port_config, "storm_control"));
  char command[64];
  snprintf(command, sizeof(command), "PORT %u, ENABLED %s", port,
           enabled ? "true" : "false");
  return write_handler(port, "set_port_storm_control", "storm_control",
                       command, false, result);
}

static int apply_vlan(unsigned int port, struct json_object *port_config,
                      struct apply_result *result) {
  struct json_object *vlan = json_object_object_get(port_config, "vlan");
  const char *mode = json_object_get_string(json_object_object_get(vlan, "mode"));
  int pvid = json_object_get_int(json_object_object_get(vlan, "pvid"));
  const char *allowed = json_object_get_string(
      json_object_object_get(vlan, "allowed"));
  int untagged = json_object_get_int(
      json_object_object_get(vlan, "untagged_vid"));
  bool ingress = json_object_get_boolean(
      json_object_object_get(vlan, "ingress_filter"));
  bool tagged = strcmp(mode, "access") != 0;
  char access_allowed[16];
  if (!tagged) {
    snprintf(access_allowed, sizeof(access_allowed), "%d", pvid);
    allowed = access_allowed;
    untagged = pvid;
  }
  char command[512];
  snprintf(command, sizeof(command),
      "PORT %u, ALLOWED_VLANS %s, ALLOW_TAGGED_IN %s, "
      "ALLOW_UNTAGGED_IN true, INGRESS_FILTER %s, "
      "RADIUS_CAN_ASSIGN_VLAN false, PVID %d, UNTAGGED_VID %d",
      port, allowed, tagged ? "true" : "false",
      ingress ? "true" : "false", pvid, untagged);
  return write_handler(port, "set_vlan_allports_conf", "vlan", command,
                       true, result);
}

static int apply_stp(unsigned int port, struct json_object *port_config,
                     struct apply_result *result) {
  if (!meraki_mac[0]) {
    apply_result_fail(result, "port %u STP failed: switch MAC unavailable", port);
    return -ENOENT;
  }
  struct json_object *stp = json_object_object_get(port_config, "stp");
  bool enabled = json_object_get_boolean(json_object_object_get(stp, "enabled"));
  bool auto_edge = json_object_get_boolean(
      json_object_object_get(stp, "auto_edge"));
  bool edge = json_object_get_boolean(json_object_object_get(stp, "edge"));
  int priority = json_object_get_int(json_object_object_get(stp, "priority"));
  int cost = json_object_get_int(json_object_object_get(stp, "cost"));
  char command[256];
  snprintf(command, sizeof(command),
      "PORT %s/%u, ENABLED %s, AUTOEDGE %s, EDGE %s, "
      "AUTOPTP true, PTP false, PRI %d, COST %d",
      meraki_mac, port, enabled ? "true" : "false",
      auto_edge ? "true" : "false", edge ? "true" : "false",
      priority, cost);
  log_change(port, "stp", command);
  int rc = dry_run ? 0 : click_write("/click/stp/set_many_port_cfgs", command);
  if (rc != 0) apply_result_fail(result, "port %u STP failed: %s", port,
                                 strerror(-rc));
  else apply_result_applied(result);
  return rc;
}

static int apply_poe(unsigned int port, struct json_object *port_config,
                     struct apply_result *result) {
  if (!hardware_port_supports_poe(&hardware, port)) return 0;
  if (!hardware.poe_available) {
    apply_result_fail(result,
        "port %u PoE failed: controller unavailable", port);
    return -ENODEV;
  }
  struct json_object *poe = json_object_object_get(port_config, "poe");
  bool enabled = json_object_get_boolean(json_object_object_get(poe, "enabled"));
  const char *mode = json_object_get_string(json_object_object_get(poe, "mode"));
  int desired_mode = !strcmp(mode, "af") ? PORT_MODE_AF : PORT_MODE_AT;

  log_change(port, "poe.mode", mode);
  int rc = dry_run ? 0 : port_set_type(&pd690xx, (int)port, desired_mode);
  if (rc != 0) {
    apply_result_fail(result, "port %u PoE mode %s failed", port, mode);
  } else {
    apply_result_applied(result);
  }

  log_change(port, "poe.enabled", enabled ? "true" : "false");
  int enabled_rc = dry_run ? 0 :
      (enabled ? port_enable(&pd690xx, (int)port)
               : port_disable(&pd690xx, (int)port));
  if (enabled_rc != 0)
    apply_result_fail(result, "port %u PoE %s failed", port,
                      enabled ? "enable" : "disable");
  else
    apply_result_applied(result);
  return rc != 0 ? rc : enabled_rc;
}

typedef int (*apply_fn)(unsigned int, struct json_object *,
                        struct apply_result *);

struct field_apply {
  const char *key;
  apply_fn apply;
};

static const struct field_apply fields[] = {
  {"enabled", apply_phy}, {"speed", apply_phy},
  {"flow_control", apply_phy}, {"eee", apply_phy},
  {"storm_control", apply_storm}, {"vlan", apply_vlan},
  {"stp", apply_stp}, {"poe", apply_poe},
};

static int apply_port_fields(unsigned int port,
                             struct json_object *full_port,
                             struct json_object *changed_port,
                             struct apply_result *result) {
  apply_fn applied[8] = {0};
  size_t applied_count = 0;
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
    struct json_object *changed;
    if (changed_port &&
        !json_object_object_get_ex(changed_port, fields[i].key, &changed))
      continue;
    bool duplicate = false;
    for (size_t j = 0; j < applied_count; j++)
      if (applied[j] == fields[i].apply) duplicate = true;
    if (duplicate) continue;
    int rc = fields[i].apply(port, full_port, result);
    if (rc != 0) return rc;
    applied[applied_count++] = fields[i].apply;
  }
  return 0;
}

static int apply_ports(struct json_object *full_config,
                       struct json_object *delta,
                       struct apply_result *result) {
  struct json_object *full_ports = NULL;
  if (!json_object_object_get_ex(full_config, "ports", &full_ports)) return 0;
  struct json_object *changed_ports = NULL;
  if (delta && !json_object_object_get_ex(delta, "ports", &changed_ports))
    return 0;

  struct json_object *iterate = delta ? changed_ports : full_ports;
  json_object_object_foreach(iterate, key, changed_port) {
    struct json_object *full_port = NULL;
    if (!json_object_object_get_ex(full_ports, key, &full_port)) continue;
    unsigned int port = (unsigned int)strtoul(key, NULL, 10);
    int rc = apply_port_fields(port, full_port,
                               delta ? changed_port : NULL, result);
    if (rc != 0) return rc;
  }
  return 0;
}

int click_apply_ports_full(struct json_object *config,
                           struct apply_result *result) {
  return apply_ports(config, NULL, result);
}

int click_apply_ports_delta(struct json_object *full_config,
                            struct json_object *delta,
                            struct apply_result *result) {
  return apply_ports(full_config, delta, result);
}
