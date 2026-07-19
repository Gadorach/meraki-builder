#include "status.h"
#include "config_file.h"
#include "configd.h"
#include "network.h"
#include "release.h"
#include "service_ops.h"
#include "time_ops.h"
#include "system_info.h"
#include "system_identity.h"
#include "compatibility.h"
#include "auth.h"

#include <libpostmerkos.h>
#include <libpd690xx.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CLIENTS_FILE "/click/client_ip_table/list"

static void add_error(struct json_object *errors, const char *source,
                      const char *message) {
  struct json_object *error = json_object_new_object();
  json_object_object_add(error, "source", json_object_new_string(source));
  json_object_object_add(error, "message", json_object_new_string(message));
  json_object_array_add(errors, error);
}

static void add_temperatures(struct json_object *root,
                             struct json_object *errors) {
  struct json_object *temperature = json_object_new_object();
  struct json_object *cpu = json_object_new_array();
  json_object_object_add(temperature, "cpu", cpu);
  json_object_object_add(root, "temperature", temperature);

  const char *thermal_path = getenv("CONFIGD_THERMAL_PATH");
  if (!thermal_path || !*thermal_path) thermal_path = "/sys/class/thermal";
  DIR *directory = opendir(thermal_path);
  if (!directory) {
    add_error(errors, "temperature", "thermal subsystem unavailable");
  } else {
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
      if (!starts_with(entry->d_name, "thermal_zone")) continue;
      char path[256];
      int length = snprintf(path, sizeof(path), "%s/%s/temp",
                            thermal_path, entry->d_name);
      if (length < 0 || (size_t)length >= sizeof(path)) continue;
      FILE *file = fopen(path, "r");
      if (!file) continue;
      long millidegrees = 0;
      if (fscanf(file, "%ld", &millidegrees) == 1)
        json_object_array_add(cpu,
            json_object_new_double((double)millidegrees / 1000.0));
      fclose(file);
    }
    closedir(directory);
  }

  if (hardware.poe_available) {
    struct json_object *poe = json_object_new_array();
    json_object_object_add(temperature, "poe", poe);
    float *values = get_temp(&pd690xx);
    if (!values) {
      add_error(errors, "poe", "PoE temperature read failed");
    } else {
      for (unsigned int i = 0; i < hardware.poe_controller_count; i++)
        json_object_array_add(poe, json_object_new_double(values[i]));
      free(values);
    }
  }
}

static void add_port_status(struct json_object *root,
                            struct json_object *errors) {
  struct json_object *ports = json_object_new_object();
  json_object_object_add(root, "ports", ports);

  const char *ports_path = getenv("CONFIGD_PORTS_FILE");
  if (!ports_path || !*ports_path) ports_path = PORTS_FILE;
  FILE *file = fopen(ports_path, "r");
  if (!file) {
    add_error(errors, "ports", "Click port status handler unavailable");
    return;
  }

  char line[512];
  unsigned int port = 0;
  bool header = true;
  while (fgets(line, sizeof(line), file)) {
    if (header) { header = false; continue; }
    port++;
    char established[32] = "0";
    char speed[32] = "0";
    if (get_field_copy(line, 2, established, sizeof(established)) != 0 ||
        get_field_copy(line, 3, speed, sizeof(speed)) != 0) {
      char message[96];
      snprintf(message, sizeof(message), "port %u status row is malformed", port);
      add_error(errors, "ports", message);
      continue;
    }

    char key[16];
    snprintf(key, sizeof(key), "%u", port);
    struct json_object *port_status = json_object_new_object();
    struct json_object *link = json_object_new_object();
    json_object_object_add(link, "established",
        json_object_new_boolean(atoi(established) != 0));
    json_object_object_add(link, "speed", json_object_new_int(atoi(speed)));
    json_object_object_add(port_status, "link", link);

    struct json_object *capabilities = json_object_new_object();
    bool poe_supported = hardware_port_supports_poe(&hardware, port);
    json_object_object_add(capabilities, "poe",
                           json_object_new_boolean(poe_supported));
    bool uplink = hardware.uplink_port_count > 0 &&
                  port > hardware.copper_port_count;
    json_object_object_add(capabilities, "media",
        json_object_new_string(uplink ? hardware.uplink_media : "copper"));
    json_object_object_add(capabilities, "max_speed_mbps",
        json_object_new_int(uplink ? (int)hardware.uplink_max_speed_mbps : 1000));
    json_object_object_add(port_status, "capabilities", capabilities);

    if (poe_supported) {
      struct json_object *poe = json_object_new_object();
      json_object_object_add(poe, "available",
                             json_object_new_boolean(hardware.poe_available));
      if (hardware.poe_available) {
        float power = port_power(&pd690xx, (int)port);
        if (power >= 0)
          json_object_object_add(poe, "power", json_object_new_double(power));
        else
          json_object_object_add(poe, "power", json_object_new_null());
      }
      char pruned_path[96];
      snprintf(pruned_path, sizeof(pruned_path), "/run/postmerkos/poe-pruned/%u", port);
      json_object_object_add(poe, "boot_pruned",
                             json_object_new_boolean(access(pruned_path, F_OK) == 0));
      json_object_object_add(port_status, "poe", poe);
    }
    json_object_object_add(ports, key, port_status);
  }
  fclose(file);
}

/* Report clients learned by Click's client_ip_table, grouped by switch port.
 * Each row of the "list" handler is whitespace-delimited with a header line;
 * field 3 = port, field 6 = mac, field 7 = seen_ip, field 9 = sniff_ago. */
static void add_clients(struct json_object *root, struct json_object *errors) {
  struct json_object *clients = json_object_new_object();
  json_object_object_add(root, "clients", clients);

  const char *clients_path = getenv("CONFIGD_CLIENTS_FILE");
  if (!clients_path || !*clients_path) clients_path = CLIENTS_FILE;
  FILE *file = fopen(clients_path, "r");
  if (!file) {
    add_error(errors, "clients", "Click client table handler unavailable");
    return;
  }

  char line[512];
  bool header = true;
  while (fgets(line, sizeof(line), file)) {
    if (header) { header = false; continue; }

    char port_str[16] = "";
    char mac[32] = "";
    char ip[64] = "";
    char age_str[32] = "";

    if (get_field_copy(line, 3, port_str, sizeof(port_str)) != 0 ||
        get_field_copy(line, 6, mac, sizeof(mac)) != 0 ||
        get_field_copy(line, 7, ip, sizeof(ip)) != 0)
      continue;

    get_field_copy(line, 9, age_str, sizeof(age_str));

    struct json_object *port_array;
    if (!json_object_object_get_ex(clients, port_str, &port_array)) {
      port_array = json_object_new_array();
      json_object_object_add(clients, port_str, port_array);
    }

    struct json_object *entry = json_object_new_object();
    json_object_object_add(entry, "mac", json_object_new_string(mac));
    json_object_object_add(entry, "ip", json_object_new_string(ip));
    json_object_object_add(entry, "age", json_object_new_double(atof(age_str)));
    json_object_array_add(port_array, entry);
  }
  fclose(file);
}

struct json_object *reset_button_status_json(void) {
  struct json_object *root = json_object_new_object();
  const char *status_path = getenv("POSTMERKOS_BUTTON_STATUS");
  if (!status_path || !*status_path)
    status_path = "/run/postmerkos/button-status.json";
  struct json_object *button = json_object_from_file(status_path);
  if (!button || !json_object_is_type(button, json_type_object)) {
    if (button) json_object_put(button);
    button = json_object_new_object();
    json_object_object_add(button, "state", json_object_new_string("unavailable"));
    json_object_object_add(button, "available", json_object_new_boolean(false));
    json_object_object_add(button, "pressed", json_object_new_boolean(false));
    json_object_object_add(button, "armed", json_object_new_boolean(false));
    json_object_object_add(button, "countdown_active", json_object_new_boolean(false));
    json_object_object_add(button, "led_indication_active", json_object_new_boolean(false));
    json_object_object_add(button, "detail",
                           json_object_new_string("button status has not been published"));
  }
  json_object_object_add(root, "reset_button", button);

  const char *owner_path = getenv("POSTMERKOS_LED_OWNER");
  if (!owner_path || !*owner_path) owner_path = "/run/postmerkos/led-owner";
  char owner[64] = "normal";
  FILE *led_owner = fopen(owner_path, "r");
  if (led_owner) {
    if (fgets(owner, sizeof(owner), led_owner))
      owner[strcspn(owner, "\r\n")] = '\0';
    fclose(led_owner);
  }
  if (!owner[0]) snprintf(owner, sizeof(owner), "normal");
  json_object_object_add(root, "led_owner", json_object_new_string(owner));
  return root;
}

struct json_object *get_status(void) {
  struct json_object *root = json_object_new_object();
  struct json_object *errors = json_object_new_array();
  json_object_object_add(root, "datetime", json_object_new_string(get_time()));
  json_object_object_add(root, "device", json_object_new_string(hardware.model));
  json_object_object_add(root, "release", release_info_load());
  json_object_object_add(root, "time", time_status_json());
  json_object_object_add(root, "services", service_status_json());
  json_object_object_add(root, "capabilities",
                         hardware_capabilities_json(&hardware));
  json_object_object_add(root, "compatibility_notice", compatibility_notice_json());
  json_object_object_add(root, "network", network_manager_status_json());
  json_object_object_add(root, "system", system_info_json());
  json_object_object_add(root, "identity", system_identity_status_json());
  struct json_object *security = json_object_new_object();
  /* Deterministic default-password check (root password still == device serial),
     shared with the lightweight security.default-password RPC. */
  json_object_object_add(security, "default_password_active",
                         json_object_new_boolean(auth_default_password_active()));
  const char *overlay_recovery = getenv("POSTMERKOS_OVERLAY_RECOVERY_MARKER");
  if (!overlay_recovery || !*overlay_recovery)
    overlay_recovery = "/run/postmerkos/overlay-recovery-mode";
  bool recovery_mode = access(overlay_recovery, F_OK) == 0;
  json_object_object_add(security, "persistent_overlay_recovery_mode",
                         json_object_new_boolean(recovery_mode));
  if (recovery_mode)
    add_error(errors, "persistent-overlay",
              "persistent JFFS2 is unavailable; the switch is using a temporary recovery overlay and changes will not survive reboot");
  json_object_object_add(root, "security", security);
  struct json_object *hardware_policy = json_object_new_object();
  struct json_object *hardware_controls = json_object_from_file(
      "/run/postmerkos/hardware-controls.json");
  if (hardware_controls &&
      json_object_is_type(hardware_controls, json_type_object)) {
    struct json_object *leds = NULL;
    struct json_object *status_led = NULL;
    if (json_object_object_get_ex(hardware_controls, "leds", &leds) &&
        json_object_is_type(leds, json_type_object) &&
        json_object_object_get_ex(leds, "status", &status_led) &&
        json_object_is_type(status_led, json_type_object))
      json_object_object_add(hardware_policy, "status_led",
                             json_object_get(status_led));
    struct json_object *profile_exact = NULL;
    if (json_object_object_get_ex(hardware_controls, "profile_exact",
                                  &profile_exact))
      json_object_object_add(hardware_policy, "profile_exact",
                             json_object_get(profile_exact));
    json_object_put(hardware_controls);
  } else if (hardware_controls) {
    json_object_put(hardware_controls);
  }
  struct json_object *reset_live = reset_button_status_json();
  struct json_object *button_status = NULL;
  struct json_object *led_owner = NULL;
  if (json_object_object_get_ex(reset_live, "reset_button", &button_status)) {
    json_object_object_add(hardware_policy, "reset_button",
                           json_object_get(button_status));
    struct json_object *state = NULL;
    if (json_object_object_get_ex(button_status, "state", &state) &&
        json_object_is_type(state, json_type_string) &&
        (!strcmp(json_object_get_string(state), "unidentified") ||
         !strcmp(json_object_get_string(state), "unavailable")))
      add_error(errors, "reset-button",
                "verified physical reset-button input is unavailable; inspect /run/postmerkos/hardware.log and /run/postmerkos/buttond.log");
  }
  if (json_object_object_get_ex(reset_live, "led_owner", &led_owner))
    json_object_object_add(hardware_policy, "led_owner", json_object_get(led_owner));
  json_object_put(reset_live);
  json_object_object_add(root, "hardware_policy", hardware_policy);
  add_temperatures(root, errors);
  add_port_status(root, errors);
  add_clients(root, errors);

  const char *config_error = config_file_runtime_error();
  if (config_error) add_error(errors, "configuration", config_error);
  const struct network_runtime *network = network_manager_runtime();
  if (network->last_error[0]) add_error(errors, "network", network->last_error);
  json_object_object_add(root, "errors", errors);
  return root;
}
