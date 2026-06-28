#include "validation.h"
#include "configd.h"
#include "network.h"
#include "ssh_keys.h"
#include "telemetry.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad(char *error, size_t error_size, const char *format, ...) {
  if (error && error_size) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
  }
  return -EINVAL;
}

static bool key_allowed(const char *key, const char *const *allowed,
                        size_t count) {
  for (size_t i = 0; i < count; i++) if (!strcmp(key, allowed[i])) return true;
  return false;
}

static int reject_unknown(struct json_object *object,
                          const char *const *allowed, size_t count,
                          const char *path,
                          char *error, size_t error_size) {
  if (!json_object_is_type(object, json_type_object))
    return bad(error, error_size, "%s must be an object", path);
  json_object_object_foreach(object, key, value) {
    (void)value;
    if (!key_allowed(key, allowed, count))
      return bad(error, error_size, "%s.%s is not supported", path, key);
  }
  return 0;
}

static int require_bool(struct json_object *object, const char *key,
                        const char *path, bool required,
                        char *error, size_t error_size) {
  struct json_object *value = NULL;
  if (!json_object_object_get_ex(object, key, &value))
    return required ? bad(error, error_size, "%s.%s is required", path, key) : 0;
  if (!json_object_is_type(value, json_type_boolean))
    return bad(error, error_size, "%s.%s must be boolean", path, key);
  return 0;
}

static int require_int_range(struct json_object *object, const char *key,
                             const char *path, bool required,
                             int minimum, int maximum,
                             char *error, size_t error_size) {
  struct json_object *value = NULL;
  if (!json_object_object_get_ex(object, key, &value))
    return required ? bad(error, error_size, "%s.%s is required", path, key) : 0;
  if (!json_object_is_type(value, json_type_int))
    return bad(error, error_size, "%s.%s must be an integer", path, key);
  int number = json_object_get_int(value);
  if (number < minimum || number > maximum)
    return bad(error, error_size, "%s.%s must be between %d and %d",
               path, key, minimum, maximum);
  return 0;
}

static int require_string(struct json_object *object, const char *key,
                          const char *path, bool required,
                          size_t maximum_length,
                          char *error, size_t error_size) {
  struct json_object *value = NULL;
  if (!json_object_object_get_ex(object, key, &value))
    return required ? bad(error, error_size, "%s.%s is required", path, key) : 0;
  if (!json_object_is_type(value, json_type_string))
    return bad(error, error_size, "%s.%s must be a string", path, key);
  const char *text = json_object_get_string(value);
  if (strlen(text) > maximum_length)
    return bad(error, error_size, "%s.%s is too long", path, key);
  for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    if (*p < 0x20 || *p == 0x7f)
      return bad(error, error_size, "%s.%s contains control characters",
                 path, key);
  return 0;
}

static bool string_in(const char *value, const char *const *values,
                      size_t count) {
  for (size_t i = 0; i < count; i++) if (!strcmp(value, values[i])) return true;
  return false;
}

static int validate_vlan_list(const char *text, bool allow_empty,
                              char *error, size_t error_size,
                              const char *path) {
  if (!text || !*text) return allow_empty ? 0 :
      bad(error, error_size, "%s must not be empty", path);
  char copy[512];
  if (strlen(text) >= sizeof(copy))
    return bad(error, error_size, "%s is too long", path);
  snprintf(copy, sizeof(copy), "%s", text);

  char *save = NULL;
  for (char *part = strtok_r(copy, ",", &save); part;
       part = strtok_r(NULL, ",", &save)) {
    if (!*part) return bad(error, error_size, "%s contains an empty item", path);
    char *dash = strchr(part, '-');
    if (dash && strchr(dash + 1, '-'))
      return bad(error, error_size, "%s contains an invalid range", path);
    char *end = NULL;
    unsigned long first = strtoul(part, &end, 10);
    if (end == part || (dash ? end != dash : *end) || first < 1 || first > 4094)
      return bad(error, error_size, "%s contains an invalid VLAN", path);
    if (dash) {
      unsigned long last = strtoul(dash + 1, &end, 10);
      if (end == dash + 1 || *end || last < first || last > 4094)
        return bad(error, error_size, "%s contains an invalid VLAN range", path);
    }
  }
  return 0;
}

static int validate_network_schema(struct json_object *config,
                                   char *error, size_t error_size) {
  struct json_object *network = NULL;
  struct json_object *ipv4 = NULL;
  const char *network_keys[] = {"ipv4"};
  const char *dhcp_keys[] = {"mode", "fallback_address", "mtu"};
  const char *static_keys[] = {"mode", "address", "gateway", "mtu"};
  if (!json_object_object_get_ex(config, "network", &network))
    return bad(error, error_size, "network is required");
  if (reject_unknown(network, network_keys, 1, "network",
                     error, error_size) != 0) return -EINVAL;
  if (!json_object_object_get_ex(network, "ipv4", &ipv4) ||
      !json_object_is_type(ipv4, json_type_object))
    return bad(error, error_size, "network.ipv4 must be an object");
  struct json_object *mode = NULL;
  if (!json_object_object_get_ex(ipv4, "mode", &mode) ||
      !json_object_is_type(mode, json_type_string))
    return bad(error, error_size, "network.ipv4.mode must be a string");
  const char *text = json_object_get_string(mode);
  const char *const *keys = !strcmp(text, "static") ? static_keys : dhcp_keys;
  size_t key_count = !strcmp(text, "static") ? 4 : 3;
  if (reject_unknown(ipv4, keys, key_count, "network.ipv4",
                     error, error_size) != 0) return -EINVAL;
  return network_validate_config(config, error, error_size);
}

static int validate_vlan(struct json_object *vlan, const char *path,
                         char *error, size_t error_size) {
  const char *keys[] = {"mode", "pvid", "allowed", "untagged_vid",
                        "ingress_filter"};
  if (reject_unknown(vlan, keys, 5, path, error, error_size) != 0) return -EINVAL;
  if (require_string(vlan, "mode", path, true, 8, error, error_size) != 0)
    return -EINVAL;
  const char *mode = json_object_get_string(json_object_object_get(vlan, "mode"));
  const char *modes[] = {"access", "trunk", "hybrid"};
  if (!string_in(mode, modes, 3))
    return bad(error, error_size, "%s.mode must be access, trunk, or hybrid", path);
  if (require_int_range(vlan, "pvid", path, true, 1, 4094,
                        error, error_size) != 0 ||
      require_int_range(vlan, "untagged_vid", path, true, 0, 4094,
                        error, error_size) != 0 ||
      require_bool(vlan, "ingress_filter", path, true,
                   error, error_size) != 0 ||
      require_string(vlan, "allowed", path, true, 511,
                     error, error_size) != 0) return -EINVAL;
  const char *allowed = json_object_get_string(json_object_object_get(vlan, "allowed"));
  return validate_vlan_list(allowed, !strcmp(mode, "access"),
                            error, error_size, "vlan.allowed");
}

static int validate_port_stp(struct json_object *stp, const char *path,
                             char *error, size_t error_size) {
  const char *keys[] = {"enabled", "priority", "cost", "edge", "auto_edge"};
  if (reject_unknown(stp, keys, 5, path, error, error_size) != 0) return -EINVAL;
  if (require_bool(stp, "enabled", path, true, error, error_size) != 0 ||
      require_int_range(stp, "priority", path, true, 0, 255,
                        error, error_size) != 0 ||
      require_int_range(stp, "cost", path, true, 0, 200000000,
                        error, error_size) != 0 ||
      require_bool(stp, "edge", path, true, error, error_size) != 0 ||
      require_bool(stp, "auto_edge", path, true, error, error_size) != 0)
    return -EINVAL;
  return 0;
}

static int validate_poe(struct json_object *poe, unsigned int port,
                        const char *path, char *error, size_t error_size) {
  const char *keys[] = {"enabled", "mode", "policy", "observation_seconds"};
  if (!hardware_port_supports_poe(&hardware, port))
    return bad(error, error_size, "%s is not supported by port %u", path, port);
  if (reject_unknown(poe, keys, 4, path, error, error_size) != 0) return -EINVAL;
  if (require_bool(poe, "enabled", path, true, error, error_size) != 0 ||
      require_string(poe, "mode", path, true, 2, error, error_size) != 0)
    return -EINVAL;
  const char *mode = json_object_get_string(json_object_object_get(poe, "mode"));
  if (strcmp(mode, "af") && strcmp(mode, "at"))
    return bad(error, error_size, "%s.mode must be af or at", path);
  struct json_object *policy = NULL;
  if (json_object_object_get_ex(poe, "policy", &policy)) {
    if (!json_object_is_type(policy, json_type_string))
      return bad(error, error_size, "%s.policy must be a string", path);
    const char *value = json_object_get_string(policy);
    if (strcmp(value, "normal") && strcmp(value, "boot-prune"))
      return bad(error, error_size, "%s.policy must be normal or boot-prune", path);
  }
  if (json_object_object_get_ex(poe, "observation_seconds", &policy) &&
      (!json_object_is_type(policy, json_type_int) ||
       json_object_get_int(policy) < 30 || json_object_get_int(policy) > 3600))
    return bad(error, error_size, "%s.observation_seconds must be 30-3600", path);
  return 0;
}

static int validate_ports(struct json_object *config,
                          char *error, size_t error_size) {
  struct json_object *ports = NULL;
  if (!json_object_object_get_ex(config, "ports", &ports) ||
      !json_object_is_type(ports, json_type_object))
    return bad(error, error_size, "ports must be an object");
  const char *port_keys[] = {"enabled", "name", "speed", "flow_control",
                             "eee", "storm_control", "vlan", "stp", "poe"};
  const char *speeds[] = {"auto", "10half", "10full", "100half",
                          "100full", "1000full"};
  json_object_object_foreach(ports, port_key, port) {
    char *end = NULL;
    unsigned long number = strtoul(port_key, &end, 10);
    if (!*port_key || !end || *end || number == 0 || number > 128 ||
        !hardware_port_valid(&hardware, (unsigned int)number))
      return bad(error, error_size, "ports.%s is not a valid hardware port", port_key);
    char path[64];
    snprintf(path, sizeof(path), "ports.%s", port_key);
    if (reject_unknown(port, port_keys, 9, path, error, error_size) != 0)
      return -EINVAL;
    if (require_bool(port, "enabled", path, true, error, error_size) != 0 ||
        require_string(port, "name", path, false, 64, error, error_size) != 0 ||
        require_string(port, "speed", path, true, 16, error, error_size) != 0 ||
        require_bool(port, "flow_control", path, true, error, error_size) != 0 ||
        require_bool(port, "eee", path, true, error, error_size) != 0 ||
        require_bool(port, "storm_control", path, true, error, error_size) != 0)
      return -EINVAL;
    const char *speed = json_object_get_string(json_object_object_get(port, "speed"));
    if (!string_in(speed, speeds, 6))
      return bad(error, error_size, "%s.speed is not supported", path);

    struct json_object *nested = NULL;
    if (!json_object_object_get_ex(port, "vlan", &nested) ||
        !json_object_is_type(nested, json_type_object))
      return bad(error, error_size, "%s.vlan must be an object", path);
    char nested_path[80];
    snprintf(nested_path, sizeof(nested_path), "%s.vlan", path);
    if (validate_vlan(nested, nested_path, error, error_size) != 0) return -EINVAL;
    if (!json_object_object_get_ex(port, "stp", &nested) ||
        !json_object_is_type(nested, json_type_object))
      return bad(error, error_size, "%s.stp must be an object", path);
    snprintf(nested_path, sizeof(nested_path), "%s.stp", path);
    if (validate_port_stp(nested, nested_path, error, error_size) != 0)
      return -EINVAL;
    if (json_object_object_get_ex(port, "poe", &nested)) {
      if (!json_object_is_type(nested, json_type_object))
        return bad(error, error_size, "%s.poe must be an object", path);
      snprintf(nested_path, sizeof(nested_path), "%s.poe", path);
      if (validate_poe(nested, (unsigned int)number, nested_path,
                       error, error_size) != 0) return -EINVAL;
    }
  }
  return 0;
}

static int validate_globals(struct json_object *config,
                            char *error, size_t error_size) {
  struct json_object *value = NULL;
  const char *stp_keys[] = {"priority", "hello_time", "forward_delay",
                            "max_age", "hold_count"};
  if (!json_object_object_get_ex(config, "stp", &value) ||
      reject_unknown(value, stp_keys, 5, "stp", error, error_size) != 0)
    return bad(error, error_size, "stp must be a valid object");
  if (require_int_range(value, "priority", "stp", true, 0, 61440,
                        error, error_size) != 0 ||
      require_int_range(value, "hello_time", "stp", true, 1, 10,
                        error, error_size) != 0 ||
      require_int_range(value, "forward_delay", "stp", true, 4, 30,
                        error, error_size) != 0 ||
      require_int_range(value, "max_age", "stp", true, 6, 40,
                        error, error_size) != 0 ||
      require_int_range(value, "hold_count", "stp", true, 1, 10,
                        error, error_size) != 0) return -EINVAL;

  const char *lacp_keys[] = {"enabled"};
  if (!json_object_object_get_ex(config, "lacp", &value) ||
      reject_unknown(value, lacp_keys, 1, "lacp", error, error_size) != 0 ||
      require_bool(value, "enabled", "lacp", true, error, error_size) != 0)
    return bad(error, error_size, "lacp must contain enabled");

  const char *mc_keys[] = {"igmp_snooping", "igmp_querier_interval",
                           "mld_snooping", "mld_querier_interval"};
  if (!json_object_object_get_ex(config, "multicast", &value) ||
      reject_unknown(value, mc_keys, 4, "multicast", error, error_size) != 0)
    return bad(error, error_size, "multicast must be a valid object");
  if (require_bool(value, "igmp_snooping", "multicast", true,
                   error, error_size) != 0 ||
      require_int_range(value, "igmp_querier_interval", "multicast", true,
                        1, 3600, error, error_size) != 0 ||
      require_bool(value, "mld_snooping", "multicast", true,
                   error, error_size) != 0 ||
      require_int_range(value, "mld_querier_interval", "multicast", true,
                        1, 3600, error, error_size) != 0) return -EINVAL;
  return 0;
}

static int validate_ssh_schema(struct json_object *config,
                               char *error, size_t error_size) {
  struct json_object *ssh = NULL;
  if (!json_object_object_get_ex(config, "ssh", &ssh)) return 0;
  if (!json_object_is_type(ssh, json_type_object))
    return bad(error, error_size, "ssh must be an object");
  const char *ssh_keys[] = {"authorized_keys"};
  if (reject_unknown(ssh, ssh_keys, 1, "ssh", error, error_size) != 0)
    return -EINVAL;
  struct json_object *arr = NULL;
  if (!json_object_object_get_ex(ssh, "authorized_keys", &arr)) return 0;
  if (!json_object_is_type(arr, json_type_array))
    return bad(error, error_size, "ssh.authorized_keys must be an array");
  for (size_t i = 0; i < json_object_array_length(arr); i++) {
    struct json_object *e = json_object_array_get_idx(arr, i), *jk = NULL, *jl = NULL;
    if (!json_object_is_type(e, json_type_object))
      return bad(error, error_size, "ssh.authorized_keys entries must be objects");
    const char *entry_keys[] = {"label", "key"};
    if (reject_unknown(e, entry_keys, 2, "ssh.authorized_keys[]",
                       error, error_size) != 0) return -EINVAL;
    if (json_object_object_get_ex(e, "label", &jl) &&
        !json_object_is_type(jl, json_type_string))
      return bad(error, error_size, "ssh.authorized_keys[].label must be a string");
    if (!json_object_object_get_ex(e, "key", &jk) ||
        !json_object_is_type(jk, json_type_string))
      return bad(error, error_size, "ssh.authorized_keys[].key must be a string");
    char key_error[200];
    if (ssh_key_validate(json_object_get_string(jk), key_error,
                         sizeof(key_error)) != 0)
      return bad(error, error_size, key_error);
  }
  return 0;
}

int validate_configuration(struct json_object *config,
                           char *error, size_t error_size) {
  if (!config || !json_object_is_type(config, json_type_object))
    return bad(error, error_size, "configuration must be a JSON object");
  const char *keys[] = {"network", "ports", "stp", "lacp", "multicast", "ssh", "telemetry"};
  if (reject_unknown(config, keys, 6, "configuration",
                     error, error_size) != 0) return -EINVAL;
  if (validate_network_schema(config, error, error_size) != 0 ||
      validate_ports(config, error, error_size) != 0 ||
      validate_globals(config, error, error_size) != 0 ||
      validate_ssh_schema(config, error, error_size) != 0) return -EINVAL;
      telemetry_validate(config, error, error_size) != 0) return -EINVAL;
  return 0;
}
