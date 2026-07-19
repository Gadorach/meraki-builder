#include "console_cli.h"

#include "configd.h"
#include "network.h"
#include "status.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_LIMIT 256

static void set_error(char *error, size_t size, const char *message) {
  if (error && size) snprintf(error, size, "%s", message ? message : "error");
}

static struct json_object *object_member(struct json_object *object,
                                         const char *key) {
  struct json_object *value = NULL;
  if (!object || !key || !json_object_is_type(object, json_type_object) ||
      !json_object_object_get_ex(object, key, &value)) return NULL;
  return value;
}

static const char *string_member(struct json_object *object, const char *key,
                                 const char *fallback) {
  struct json_object *value = object_member(object, key);
  return value && json_object_is_type(value, json_type_string)
      ? json_object_get_string(value) : fallback;
}

static int int_member(struct json_object *object, const char *key,
                      int fallback) {
  struct json_object *value = object_member(object, key);
  return value && json_object_is_type(value, json_type_int)
      ? json_object_get_int(value) : fallback;
}

static bool bool_member(struct json_object *object, const char *key,
                        bool fallback) {
  struct json_object *value = object_member(object, key);
  return value && json_object_is_type(value, json_type_boolean)
      ? json_object_get_boolean(value) : fallback;
}

static const char *on_off(bool enabled) { return enabled ? "enabled" : "disabled"; }
static const char *yes_no(bool value) { return value ? "yes" : "no"; }

struct json_object *console_path_get(struct json_object *root,
                                     const char *path) {
  if (!root || !path || !*path || strlen(path) >= PATH_LIMIT ||
      path[0] == '.' || path[strlen(path) - 1] == '.' || strstr(path, ".."))
    return NULL;
  char copy[PATH_LIMIT];
  snprintf(copy, sizeof(copy), "%s", path);
  struct json_object *current = root;
  char *save = NULL;
  for (char *part = strtok_r(copy, ".", &save); part;
       part = strtok_r(NULL, ".", &save)) {
    if (!*part || !json_object_is_type(current, json_type_object) ||
        !json_object_object_get_ex(current, part, &current)) return NULL;
  }
  return current;
}

static struct json_object *parse_value(const char *text) {
  if (!text) return NULL;
  if (!*text) return json_object_new_string("");

  enum json_tokener_error parse_error = json_tokener_success;
  struct json_object *value = json_tokener_parse_verbose(text, &parse_error);
  if (parse_error == json_tokener_success && value) return value;
  if (value) json_object_put(value);
  return json_object_new_string(text);
}

static struct json_object *delta_with_value(const char *path,
                                            struct json_object *value,
                                            char *error,
                                            size_t error_size) {
  if (!path || !*path || strlen(path) >= PATH_LIMIT || path[0] == '.' ||
      path[strlen(path) - 1] == '.' || strstr(path, "..")) {
    if (value) json_object_put(value);
    set_error(error, error_size, "configuration path is invalid or too long");
    return NULL;
  }
  if (!value) {
    set_error(error, error_size, "unable to allocate configuration value");
    return NULL;
  }
  char copy[PATH_LIMIT];
  snprintf(copy, sizeof(copy), "%s", path);
  struct json_object *root = json_object_new_object();
  struct json_object *current = root;
  char *save = NULL;
  char *part = strtok_r(copy, ".", &save);
  if (!root || !part || !*part) {
    if (root) json_object_put(root);
    json_object_put(value);
    set_error(error, error_size, "configuration path is empty");
    return NULL;
  }
  while (part) {
    char *next = strtok_r(NULL, ".", &save);
    if (!*part) {
      json_object_put(root);
      json_object_put(value);
      set_error(error, error_size, "configuration path contains an empty component");
      return NULL;
    }
    if (!next) {
      json_object_object_add(current, part, value);
      return root;
    }
    struct json_object *child = json_object_new_object();
    if (!child) {
      json_object_put(root);
      json_object_put(value);
      set_error(error, error_size, "unable to allocate configuration delta");
      return NULL;
    }
    json_object_object_add(current, part, child);
    current = child;
    part = next;
  }
  json_object_put(root);
  json_object_put(value);
  set_error(error, error_size, "configuration path is invalid");
  return NULL;
}

struct json_object *console_delta_from_path(const char *path,
                                            const char *value_text,
                                            char *error,
                                            size_t error_size) {
  return delta_with_value(path, parse_value(value_text), error, error_size);
}

struct json_object *console_delta_from_string_path(const char *path,
                                                   const char *value_text,
                                                   char *error,
                                                   size_t error_size) {
  return delta_with_value(path, json_object_new_string(value_text ? value_text : ""),
                          error, error_size);
}

int console_print_path(struct json_object *config, const char *path) {
  struct json_object *value = console_path_get(config, path);
  if (!value) return -ENOENT;
  switch (json_object_get_type(value)) {
    case json_type_string:
      puts(json_object_get_string(value));
      break;
    case json_type_boolean:
      puts(json_object_get_boolean(value) ? "true" : "false");
      break;
    case json_type_int:
      printf("%lld\n", (long long)json_object_get_int64(value));
      break;
    case json_type_double:
      printf("%.6g\n", json_object_get_double(value));
      break;
    case json_type_null:
      puts("null");
      break;
    default:
      puts(json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN));
      break;
  }
  return 0;
}

static void print_temperatures(struct json_object *status) {
  struct json_object *temperature = object_member(status, "temperature");
  if (!temperature) return;
  const char *names[] = {"cpu", "poe"};
  for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
    struct json_object *values = object_member(temperature, names[n]);
    if (!values || !json_object_is_type(values, json_type_array) ||
        json_object_array_length(values) == 0) continue;
    printf("  %-18s", !strcmp(names[n], "cpu") ? "CPU temperature:" : "PoE temperature:");
    for (size_t i = 0; i < json_object_array_length(values); i++) {
      struct json_object *entry = json_object_array_get_idx(values, i);
      printf("%s%.1f C", i ? ", " : "", json_object_get_double(entry));
    }
    putchar('\n');
  }
}

int console_print_summary(struct json_object *config) {
  network_manager_observe(config);
  struct json_object *status = get_status();
  if (!status) return -ENOMEM;

  struct json_object *ports = object_member(status, "ports");
  unsigned int up = 0;
  unsigned int total = hardware.port_count;
  if (ports && json_object_is_type(ports, json_type_object)) {
    json_object_object_foreach(ports, key, port_status) {
      (void)key;
      struct json_object *link = object_member(port_status, "link");
      if (bool_member(link, "established", false)) up++;
    }
  }

  struct json_object *network = object_member(status, "network");
  struct json_object *ipv4 = object_member(network, "ipv4");
  struct json_object *errors = object_member(status, "errors");

  printf("Model:              %s\n", hardware.model[0] ? hardware.model : "unknown");
  printf("MAC address:        %s\n", meraki_mac[0] ? meraki_mac : "unknown");
  printf("Ports:              %u total, %u link up, %u link down\n",
         total, up, total >= up ? total - up : 0);
  printf("PoE:                %s", hardware.poe_supported ? "supported" : "not supported");
  if (hardware.poe_supported)
    printf(", %s, %u ports\n", hardware.poe_available ? "controller online" : "controller unavailable",
           hardware.poe_port_count);
  else putchar('\n');
  printf("Management mode:    %s\n", string_member(ipv4, "configured_mode", "unknown"));
  printf("Management source:  %s\n", string_member(ipv4, "source", "unknown"));
  printf("Management state:   %s\n", string_member(ipv4, "state", "unknown"));
  printf("Management address: %s\n", string_member(ipv4, "address", "unavailable"));
  printf("Gateway:            %s\n", string_member(ipv4, "gateway", "unavailable"));
  printf("MTU:                %d\n", int_member(ipv4, "mtu", 0));
  struct json_object *boot = object_member(status, "boot");
  if (bool_member(boot, "live_mode", false)) {
    printf("Boot mode:          PMOSLIVE RAM (volatile)\n");
    puts("WARNING: Firmware is running from RAM. Changes will not survive a reboot.");
  } else {
    printf("Boot mode:          %s\n", string_member(boot, "mode", "flash"));
  }
  print_temperatures(status);
  printf("Reported errors:    %zu\n", errors && json_object_is_type(errors, json_type_array)
         ? json_object_array_length(errors) : 0U);
  if (errors && json_object_is_type(errors, json_type_array)) {
    for (size_t i = 0; i < json_object_array_length(errors); i++) {
      struct json_object *entry = json_object_array_get_idx(errors, i);
      printf("  - %s: %s\n", string_member(entry, "source", "unknown"),
             string_member(entry, "message", "unknown error"));
    }
  }
  json_object_put(status);
  return 0;
}

static struct json_object *port_config(struct json_object *config,
                                       unsigned int port) {
  struct json_object *ports = object_member(config, "ports");
  if (!ports) return NULL;
  char key[16];
  snprintf(key, sizeof(key), "%u", port);
  return object_member(ports, key);
}

static struct json_object *port_runtime(struct json_object *status,
                                        unsigned int port) {
  struct json_object *ports = object_member(status, "ports");
  if (!ports) return NULL;
  char key[16];
  snprintf(key, sizeof(key), "%u", port);
  return object_member(ports, key);
}

int console_print_ports(struct json_object *config,
                        unsigned int first, unsigned int last) {
  if (!first || last < first || (hardware.port_count && last > hardware.port_count))
    return -EINVAL;
  network_manager_observe(config);
  struct json_object *status = get_status();
  if (!status) return -ENOMEM;
  puts("PORT  LINK   SPEED  ADMIN     POE          VLAN  MODE    NAME");
  puts("----  -----  -----  --------  -----------  ----  ------  ------------------------");
  for (unsigned int port = first; port <= last; port++) {
    struct json_object *desired = port_config(config, port);
    struct json_object *runtime = port_runtime(status, port);
    struct json_object *link = object_member(runtime, "link");
    struct json_object *vlan = object_member(desired, "vlan");
    struct json_object *poe = object_member(desired, "poe");
    char poe_text[24] = "n/a";
    if (poe) {
      if (!bool_member(poe, "enabled", false)) snprintf(poe_text, sizeof(poe_text), "off");
      else {
        struct json_object *runtime_poe = object_member(runtime, "poe");
        struct json_object *power = object_member(runtime_poe, "power");
        if (power && !json_object_is_type(power, json_type_null))
          snprintf(poe_text, sizeof(poe_text), "%s %.1fW",
                   string_member(poe, "mode", "on"), json_object_get_double(power));
        else snprintf(poe_text, sizeof(poe_text), "%s",
                      string_member(poe, "mode", "on"));
      }
    }
    printf("%-4u  %-5s  %-5d  %-8s  %-11s  %-4d  %-6s  %s\n",
           port, bool_member(link, "established", false) ? "up" : "down",
           int_member(link, "speed", 0),
           bool_member(desired, "enabled", true) ? "enabled" : "disabled",
           poe_text, int_member(vlan, "pvid", 1),
           string_member(vlan, "mode", "access"),
           string_member(desired, "name", ""));
  }
  json_object_put(status);
  return 0;
}

int console_print_port(struct json_object *config, unsigned int port) {
  if (!hardware_port_valid(&hardware, port)) return -EINVAL;
  network_manager_observe(config);
  struct json_object *status = get_status();
  if (!status) return -ENOMEM;
  struct json_object *desired = port_config(config, port);
  struct json_object *runtime = port_runtime(status, port);
  if (!desired) { json_object_put(status); return -ENOENT; }

  struct json_object *link = object_member(runtime, "link");
  struct json_object *vlan = object_member(desired, "vlan");
  struct json_object *stp = object_member(desired, "stp");
  struct json_object *runtime_stp = object_member(runtime, "stp");
  struct json_object *poe = object_member(desired, "poe");
  struct json_object *runtime_poe = object_member(runtime, "poe");
  struct json_object *power = object_member(runtime_poe, "power");

  printf("Port %u\n", port);
  printf("  Name:              %s\n", string_member(desired, "name", ""));
  printf("  Administrative:    %s\n", on_off(bool_member(desired, "enabled", true)));
  printf("  Link:              %s", bool_member(link, "established", false) ? "up" : "down");
  if (int_member(link, "speed", 0)) printf(" at %d Mbps", int_member(link, "speed", 0));
  putchar('\n');
  printf("  Configured speed:  %s\n", string_member(desired, "speed", "auto"));
  printf("  Flow control:      %s\n", on_off(bool_member(desired, "flow_control", false)));
  printf("  EEE:               %s\n", on_off(bool_member(desired, "eee", false)));
  printf("  Storm control:     %s\n", on_off(bool_member(desired, "storm_control", false)));
  printf("  VLAN mode:         %s\n", string_member(vlan, "mode", "access"));
  printf("  VLAN PVID:         %d\n", int_member(vlan, "pvid", 1));
  printf("  Allowed VLANs:     %s\n", string_member(vlan, "allowed", ""));
  printf("  Native VLAN:       %d\n", int_member(vlan, "untagged_vid", 0));
  printf("  Ingress filtering: %s\n", on_off(bool_member(vlan, "ingress_filter", true)));
  printf("  STP:               %s\n", on_off(bool_member(stp, "enabled", true)));
  printf("  STP priority:      %d\n", int_member(stp, "priority", 128));
  printf("  STP cost:          %d\n", int_member(stp, "cost", 0));
  printf("  STP edge:          %s\n", yes_no(bool_member(stp, "edge", false)));
  printf("  STP auto-edge:     %s\n", yes_no(bool_member(stp, "auto_edge", true)));
  if (runtime_stp) {
    printf("  STP runtime:       %s / %s\n",
           string_member(runtime_stp, "state", "unknown"),
           string_member(runtime_stp, "role", "unknown"));
  }
  if (poe) {
    printf("  PoE:               %s (%s)",
           on_off(bool_member(poe, "enabled", false)),
           string_member(poe, "mode", "at"));
    if (power && !json_object_is_type(power, json_type_null))
      printf(", %.2f W", json_object_get_double(power));
    putchar('\n');
  } else {
    puts("  PoE:               not supported");
  }
  json_object_put(status);
  return 0;
}

int console_export_config(struct json_object *config, const char *path,
                          char *error, size_t error_size) {
  if (!path || !*path) {
    set_error(error, error_size, "backup path is required");
    return -EINVAL;
  }
  mode_t old_mask = umask(0077);
  FILE *file = fopen(path, "w");
  umask(old_mask);
  if (!file) {
    set_error(error, error_size, strerror(errno));
    return -errno;
  }
  const char *json = json_object_to_json_string_ext(config,
                                                     JSON_C_TO_STRING_PRETTY);
  int rc = fprintf(file, "%s\n", json) < 0 ? -EIO : 0;
  if (rc == 0 && fflush(file) != 0) rc = -errno;
  if (rc == 0 && fsync(fileno(file)) != 0) rc = -errno;
  if (fclose(file) != 0 && rc == 0) rc = -errno;
  if (rc != 0) {
    unlink(path);
    set_error(error, error_size, strerror(-rc));
    return rc;
  }
  chmod(path, 0600);
  return 0;
}
