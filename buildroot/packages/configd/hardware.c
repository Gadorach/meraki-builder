#include "hardware.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "release.h"

struct hardware_record {
  const char *model;
  const char *family;
  unsigned int ports;
  unsigned int copper;
  unsigned int uplinks;
  unsigned int poe_ports;
  unsigned int switch_instances;
  enum compatibility_state compatibility;
};

static const struct hardware_record records[] = {
  {"MS220-8", "vcore3-luton", 10, 8, 2, 0, 1, COMPATIBILITY_UNTESTED},
  {"MS220-8P", "vcore3-luton", 10, 8, 2, 8, 1, COMPATIBILITY_UNTESTED},
  {"MS220-24", "vcore3-luton", 26, 24, 2, 0, 1, COMPATIBILITY_UNTESTED},
  {"MS220-24P", "vcore3-luton", 26, 24, 2, 24, 1, COMPATIBILITY_UNTESTED},
  {"MS220-48", "vcore3-jaguar-dual", 52, 48, 4, 0, 2, COMPATIBILITY_UNTESTED},
  {"MS220-48LP", "vcore3-jaguar-dual", 52, 48, 4, 48, 2, COMPATIBILITY_UNTESTED},
  {"MS220-48FP", "vcore3-jaguar-dual", 52, 48, 4, 48, 2, COMPATIBILITY_UNTESTED},
  {"MS220-48P", "vcore3-jaguar-dual", 52, 48, 4, 48, 2, COMPATIBILITY_UNTESTED},
  {"MS22", "vcore3-luton", 26, 24, 2, 0, 1, COMPATIBILITY_UNTESTED},
  {"MS22P", "vcore3-luton", 26, 24, 2, 24, 1, COMPATIBILITY_UNTESTED},
  {"MS42", "vcore3-jaguar-dual", 52, 48, 4, 0, 2, COMPATIBILITY_UNTESTED},
  {"MS42P", "vcore3-jaguar-dual", 52, 48, 4, 48, 2, COMPATIBILITY_UNTESTED},
  {"MS320-24", "vcore3-jaguar", 28, 24, 4, 0, 1, COMPATIBILITY_UNTESTED},
  {"MS320-24P", "vcore3-jaguar", 28, 24, 4, 24, 1, COMPATIBILITY_UNTESTED},
  {"MS320-48", "vcore3-jaguar-dual", 52, 48, 4, 0, 2, COMPATIBILITY_UNTESTED},
  {"MS320-48LP", "vcore3-jaguar-dual", 52, 48, 4, 48, 2, COMPATIBILITY_UNTESTED},
  {"MS320-48FP", "vcore3-jaguar-dual", 52, 48, 4, 48, 2, COMPATIBILITY_UNTESTED},
  {"MS320-48P", "vcore3-jaguar-dual", 52, 48, 4, 48, 2, COMPATIBILITY_UNTESTED},
};

static const char *env_or_default(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value && *value ? value : fallback;
}

static void trim(char *value) {
  if (!value) return;
  value[strcspn(value, "\r\n")] = '\0';
  while (*value == ' ' || *value == '\t') memmove(value, value + 1, strlen(value));
  size_t len = strlen(value);
  while (len && (value[len - 1] == ' ' || value[len - 1] == '\t'))
    value[--len] = '\0';
}

static void copy_model(char *model, size_t size, const char *value) {
  if (!model || !size) return;
  size_t length = value ? strlen(value) : 0;
  if (length >= size) length = size - 1;
  if (length) memcpy(model, value, length);
  model[length] = '\0';
}

static int read_model(char *model, size_t size) {
  const char *override = getenv("CONFIGD_MODEL");
  if (override && *override) { copy_model(model, size, override); return 0; }
  const char *path = env_or_default("CONFIGD_BOARDINFO", "/run/postmerkos/boardinfo");
  FILE *file = fopen(path, "r");
  if (!file) return -errno;
  char line[128];
  while (fgets(line, sizeof(line), file)) {
    trim(line);
    const char *value = !strncmp(line, "MODEL=", 6) ? line + 6 : line;
    if (!strncmp(value, "MS", 2)) {
      copy_model(model, size, value);
      fclose(file);
      return 0;
    }
  }
  fclose(file);
  return -ENOENT;
}

static const struct hardware_record *find_record(const char *model) {
  for (size_t i = 0; i < sizeof(records) / sizeof(records[0]); i++)
    if (model && !strcmp(records[i].model, model)) return &records[i];
  return NULL;
}

static unsigned int read_port_count(void) {
  const char *override = getenv("CONFIGD_PORT_COUNT");
  if (override && *override) return (unsigned int)strtoul(override, NULL, 10);
  const char *path = env_or_default("CONFIGD_NUM_PORTS", "/tmp/NUM_PORTS");
  FILE *file = fopen(path, "r");
  unsigned int count = 0;
  if (file) { if (fscanf(file, "%u", &count) != 1) count = 0; fclose(file); }
  return count;
}

const char *hardware_compatibility_name(enum compatibility_state state) {
  switch (state) {
    case COMPATIBILITY_CONFIRMED: return "confirmed";
    case COMPATIBILITY_INCOMPATIBLE: return "known-incompatible";
    default: return "untested";
  }
}

int hardware_init(struct hardware_info *info, struct pd690xx_cfg *pd690xx) {
  if (!info || !pd690xx) return -EINVAL;
  memset(info, 0, sizeof(*info));
  if (read_model(info->model, sizeof(info->model)) != 0)
    snprintf(info->model, sizeof(info->model), "unknown");
  const struct hardware_record *record = find_record(info->model);
  if (record) {
    snprintf(info->family, sizeof(info->family), "%s", record->family);
    info->port_count = record->ports;
    info->copper_port_count = record->copper;
    info->uplink_port_count = record->uplinks;
    info->poe_port_count = record->poe_ports;
    info->switch_instances = record->switch_instances;
    info->compatibility = record->compatibility;
  } else {
    snprintf(info->family, sizeof(info->family), "unknown");
    info->compatibility = COMPATIBILITY_UNTESTED;
    info->switch_instances = 1;
  }
  unsigned int detected_ports = read_port_count();
  if (!record && detected_ports) {
    info->port_count = detected_ports;
    info->copper_port_count = detected_ports;
    info->uplink_port_count = 0;
  } else if (record && detected_ports && detected_ports != record->ports) {
    fprintf(stderr, "hardware: ignoring stale port count %u for %s (expected %u)\n",
            detected_ports, info->model, record->ports);
  }
  const char *release_state = release_model_compatibility(info->model);
  if (release_state) {
    if (!strcmp(release_state, "known-incompatible"))
      info->compatibility = COMPATIBILITY_INCOMPATIBLE;
    else if (!strcmp(release_state, "validated") ||
             !strcmp(release_state, "confirmed"))
      info->compatibility = COMPATIBILITY_CONFIRMED;
    else
      info->compatibility = COMPATIBILITY_UNTESTED;
  }
  info->poe_supported = info->poe_port_count > 0;
  const char *skip_i2c = getenv("CONFIGD_SKIP_I2C");
  if (info->poe_supported && (!skip_i2c || strcmp(skip_i2c, "1"))) i2c_init(pd690xx);
  info->poe_controller_count = (unsigned int)pd690xx_pres_count(pd690xx);
  info->poe_available = info->poe_supported && info->poe_controller_count > 0;
  return 0;
}

bool hardware_port_valid(const struct hardware_info *info, unsigned int port) {
  return info && port > 0 && (!info->port_count || port <= info->port_count);
}

bool hardware_port_supports_poe(const struct hardware_info *info, unsigned int port) {
  return info && info->poe_supported && port > 0 && port <= info->poe_port_count;
}

struct json_object *hardware_capabilities_json(const struct hardware_info *info) {
  struct json_object *caps = json_object_new_object();
  struct json_object *poe = json_object_new_object();
  json_object_object_add(caps, "model", json_object_new_string(info ? info->model : "unknown"));
  json_object_object_add(caps, "family", json_object_new_string(info ? info->family : "unknown"));
  json_object_object_add(caps, "compatibility", json_object_new_string(
      info ? hardware_compatibility_name(info->compatibility) : "untested"));
  json_object_object_add(caps, "port_count", json_object_new_int(info ? (int)info->port_count : 0));
  json_object_object_add(caps, "copper_ports", json_object_new_int(info ? (int)info->copper_port_count : 0));
  json_object_object_add(caps, "uplink_ports", json_object_new_int(info ? (int)info->uplink_port_count : 0));
  json_object_object_add(caps, "switch_instances", json_object_new_int(info ? (int)info->switch_instances : 0));
  json_object_object_add(poe, "supported", json_object_new_boolean(info && info->poe_supported));
  json_object_object_add(poe, "available", json_object_new_boolean(info && info->poe_available));
  json_object_object_add(poe, "ports", json_object_new_int(info ? (int)info->poe_port_count : 0));
  json_object_object_add(poe, "controllers", json_object_new_int(info ? (int)info->poe_controller_count : 0));
  struct json_object *modes = json_object_new_array();
  json_object_array_add(modes, json_object_new_string("af"));
  json_object_array_add(modes, json_object_new_string("at"));
  json_object_object_add(poe, "modes", modes);
  struct json_object *policies = json_object_new_array();
  json_object_array_add(policies, json_object_new_string("normal"));
  json_object_array_add(policies, json_object_new_string("boot-prune"));
  json_object_object_add(poe, "policies", policies);
  json_object_object_add(caps, "poe", poe);
  const char *controls_path = getenv("POSTMERKOS_HARDWARE_CONTROLS");
  if (!controls_path || !*controls_path)
    controls_path = "/run/postmerkos/hardware-controls.json";
  struct json_object *controls = json_object_from_file(controls_path);
  if (controls && json_object_is_type(controls, json_type_object))
    json_object_object_add(caps, "controls", controls);
  else if (controls)
    json_object_put(controls);
  return caps;
}
