#include "telemetry.h"

#include "configd.h"
#include "hardware.h"
#include "metrics.h"
#include "portstats.h"
#include "service_ops.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libpostmerkos.h>
#include <libpd690xx.h>

extern struct hardware_info hardware;
extern struct pd690xx_cfg pd690xx;

static struct portstats_snapshot g_snap;
static struct device_health g_health;
static struct portstats_snapshot g_prev;
static int g_have_prev;

static struct json_object *member(struct json_object *o, const char *k) {
  struct json_object *v = NULL;
  if (!o || !json_object_is_type(o, json_type_object) ||
      !json_object_object_get_ex(o, k, &v)) return NULL;
  return v;
}

static bool bool_member(struct json_object *o, const char *k, bool fallback) {
  struct json_object *v = member(o, k);
  return v && json_object_is_type(v, json_type_boolean)
             ? json_object_get_boolean(v) : fallback;
}

static int int_member(struct json_object *o, const char *k, int fallback) {
  struct json_object *v = member(o, k);
  return v && json_object_is_type(v, json_type_int)
             ? json_object_get_int(v) : fallback;
}

static const char *str_member(struct json_object *o, const char *k,
                              const char *fallback) {
  struct json_object *v = member(o, k);
  return v && json_object_is_type(v, json_type_string)
             ? json_object_get_string(v) : fallback;
}

static int bad(char *error, size_t size, const char *format, ...) {
  if (error && size) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
  }
  return -EINVAL;
}

static const char *management_interface(void) {
  const char *value = getenv("CONFIGD_MGMT_IFACE");
  return value && *value ? value : TELEMETRY_MGMT_IFACE_DEFAULT;
}

struct json_object *telemetry_default_config(void) {
  struct json_object *telemetry = json_object_new_object();
  struct json_object *snmp = json_object_new_object();
  json_object_object_add(snmp, "enabled", json_object_new_boolean(false));
  json_object_object_add(snmp, "community", json_object_new_string(""));
  json_object_object_add(snmp, "location", json_object_new_string(""));
  json_object_object_add(snmp, "contact", json_object_new_string(""));
  json_object_object_add(snmp, "management_only", json_object_new_boolean(true));
  json_object_object_add(telemetry, "snmp", snmp);

  struct json_object *prometheus = json_object_new_object();
  json_object_object_add(prometheus, "enabled", json_object_new_boolean(false));
  json_object_object_add(prometheus, "port",
                         json_object_new_int(TELEMETRY_PROM_DEFAULT_PORT));
  json_object_object_add(prometheus, "management_only",
                         json_object_new_boolean(true));
  json_object_object_add(telemetry, "prometheus", prometheus);
  return telemetry;
}

static int key_allowed(const char *key, const char *const *allowed, size_t count) {
  for (size_t i = 0; i < count; i++)
    if (!strcmp(key, allowed[i])) return 1;
  return 0;
}

static int reject_unknown(struct json_object *object,
                          const char *const *allowed, size_t count,
                          const char *path, char *error, size_t error_size) {
  json_object_object_foreach(object, key, value) {
    (void)value;
    if (!key_allowed(key, allowed, count))
      return bad(error, error_size, "%s.%s is not supported", path, key);
  }
  return 0;
}

static int require_boolean(struct json_object *object, const char *key,
                           const char *path, char *error, size_t error_size) {
  struct json_object *value = member(object, key);
  if (value && !json_object_is_type(value, json_type_boolean))
    return bad(error, error_size, "%s.%s must be a boolean", path, key);
  return 0;
}

int telemetry_validate(struct json_object *config, char *error, size_t error_size) {
  struct json_object *telemetry = member(config, "telemetry");
  if (!telemetry) return 0;
  if (!json_object_is_type(telemetry, json_type_object))
    return bad(error, error_size, "telemetry must be an object");
  const char *keys[] = {"snmp", "prometheus"};
  if (reject_unknown(telemetry, keys, 2, "telemetry", error, error_size) != 0)
    return -EINVAL;

  struct json_object *snmp = member(telemetry, "snmp");
  if (snmp) {
    if (!json_object_is_type(snmp, json_type_object))
      return bad(error, error_size, "telemetry.snmp must be an object");
    const char *allowed[] = {
      "enabled", "community", "location", "contact", "management_only"
    };
    if (reject_unknown(snmp, allowed, 5, "telemetry.snmp", error,
                       error_size) != 0 ||
        require_boolean(snmp, "enabled", "telemetry.snmp", error,
                        error_size) != 0 ||
        require_boolean(snmp, "management_only", "telemetry.snmp", error,
                        error_size) != 0)
      return -EINVAL;

    struct json_object *community_value = member(snmp, "community");
    struct json_object *location_value = member(snmp, "location");
    struct json_object *contact_value = member(snmp, "contact");
    if (community_value && !json_object_is_type(community_value, json_type_string))
      return bad(error, error_size, "telemetry.snmp.community must be a string");
    if (location_value && !json_object_is_type(location_value, json_type_string))
      return bad(error, error_size, "telemetry.snmp.location must be a string");
    if (contact_value && !json_object_is_type(contact_value, json_type_string))
      return bad(error, error_size, "telemetry.snmp.contact must be a string");

    const char *community = str_member(snmp, "community", "");
    if (bool_member(snmp, "enabled", false) && !*community)
      return bad(error, error_size,
                 "telemetry.snmp.community is required when SNMP is enabled");
    if (strlen(community) > 64)
      return bad(error, error_size, "telemetry.snmp.community is too long");
    for (const unsigned char *p = (const unsigned char *)community; *p; p++)
      if (*p < 0x21 || *p == 0x7f)
        return bad(error, error_size,
                   "telemetry.snmp.community contains invalid characters");
    if (strlen(str_member(snmp, "location", "")) > 256)
      return bad(error, error_size, "telemetry.snmp.location is too long");
    if (strlen(str_member(snmp, "contact", "")) > 256)
      return bad(error, error_size, "telemetry.snmp.contact is too long");
  }

  struct json_object *prometheus = member(telemetry, "prometheus");
  if (prometheus) {
    if (!json_object_is_type(prometheus, json_type_object))
      return bad(error, error_size,
                 "telemetry.prometheus must be an object");
    const char *allowed[] = {"enabled", "port", "management_only"};
    if (reject_unknown(prometheus, allowed, 3, "telemetry.prometheus", error,
                       error_size) != 0 ||
        require_boolean(prometheus, "enabled", "telemetry.prometheus", error,
                        error_size) != 0 ||
        require_boolean(prometheus, "management_only", "telemetry.prometheus",
                        error, error_size) != 0)
      return -EINVAL;
    struct json_object *port_value = member(prometheus, "port");
    if (port_value) {
      if (!json_object_is_type(port_value, json_type_int))
        return bad(error, error_size,
                   "telemetry.prometheus.port must be an integer");
      int port = json_object_get_int(port_value);
      if (port < 1 || port > 65535)
        return bad(error, error_size,
                   "telemetry.prometheus.port must be between 1 and 65535");
      if (port == 80 || port == 161 || port == 162)
        return bad(error, error_size,
                   "telemetry.prometheus.port %d is reserved", port);
    }
  }
  return 0;
}

int telemetry_interval_seconds(void) {
  const char *value = getenv("CONFIGD_PORTSTATS_INTERVAL");
  int interval = value ? atoi(value) : 5;
  return interval >= 1 ? interval : 5;
}

int telemetry_server_fd(void) { return metrics_server_fd(); }

static void shell_quote(FILE *file, const char *key, const char *value) {
  fprintf(file, "%s='", key);
  for (const char *p = value ? value : ""; *p; p++) {
    if (*p == '\'') fputs("'\\''", file);
    else fputc(*p, file);
  }
  fputs("'\n", file);
}

int telemetry_write_snmpd_env(struct json_object *config, const char *bind_addr) {
  struct json_object *snmp = member(member(config, "telemetry"), "snmp");
  const char *path = getenv("CONFIGD_SNMPD_ENV");
  if (!path || !*path) path = "/run/postmerkos/snmpd.env";
  char temporary[512];
  if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >=
      (int)sizeof(temporary)) return -ENAMETOOLONG;

  FILE *file = fopen(temporary, "w");
  if (!file) return -errno;
  shell_quote(file, "SNMP_ENABLED",
              bool_member(snmp, "enabled", false) ? "1" : "0");
  shell_quote(file, "SNMP_COMMUNITY", str_member(snmp, "community", ""));
  shell_quote(file, "SNMP_LOCATION", str_member(snmp, "location", ""));
  shell_quote(file, "SNMP_CONTACT", str_member(snmp, "contact", ""));
  shell_quote(file, "SNMP_BIND", bind_addr ? bind_addr : "");
  shell_quote(file, "SNMP_BIND_DEVICE",
              bool_member(snmp, "management_only", true)
                ? management_interface() : "");
  if (fflush(file) != 0) {
    int saved = errno;
    fclose(file);
    unlink(temporary);
    return -saved;
  }
  int fd = fileno(file);
  if (fd >= 0 && fsync(fd) != 0) {
    int saved = errno;
    fclose(file);
    unlink(temporary);
    return -saved;
  }
  if (fclose(file) != 0) {
    int saved = errno;
    unlink(temporary);
    return -saved;
  }
  if (rename(temporary, path) != 0) {
    int saved = errno;
    unlink(temporary);
    return -saved;
  }
  return 0;
}

static enum port_link_state phy_admin_state(unsigned int port) {
  char line[512];
  char mode[32];
  if (read_switch_port_table("dump_port_phy_cfgs", port, line, sizeof(line)) != 0 ||
      get_field_copy(line, 2, mode, sizeof(mode)) != 0)
    return PORT_LINK_UNKNOWN;
  return strcmp(mode, "off") == 0 ? PORT_LINK_DOWN : PORT_LINK_UP;
}

void telemetry_enrich_port_status(struct portstats_snapshot *snapshot) {
  const char *path = getenv("CONFIGD_PORTS_FILE");
  if (!path || !*path) path = PORTS_FILE;
  FILE *file = fopen(path, "r");
  if (!file) return;

  char line[512];
  bool header = true;
  while (fgets(line, sizeof(line), file)) {
    if (header) {
      header = false;
      continue;
    }
    char port_field[32];
    char established[32];
    char speed[32];
    if (get_field_copy(line, 1, port_field, sizeof(port_field)) != 0 ||
        get_field_copy(line, 2, established, sizeof(established)) != 0 ||
        get_field_copy(line, 3, speed, sizeof(speed)) != 0)
      continue;
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(port_field, &end, 10);
    if (errno || !end || *end || parsed < 1 || parsed > PORTSTATS_MAX_PORTS)
      continue;
    unsigned int port = (unsigned int)parsed;
    struct port_counters *counter = &snapshot->ports[port - 1];
    if (!counter->present) {
      counter->present = 1;
      counter->port = (int)port;
      snapshot->count++;
    }
    counter->oper = atoi(established) != 0 ? PORT_LINK_UP : PORT_LINK_DOWN;
    counter->speed_mbps = atoi(speed);
    counter->admin = phy_admin_state(port);
    if (hardware.poe_available) {
      float watts = port_power(&pd690xx, (int)port);
      if (watts >= 0) {
        counter->poe_present = 1;
        counter->poe_power_watts = watts;
      }
    }
  }
  fclose(file);
}

static void collect_health(struct device_health *health) {
  memset(health, 0, sizeof(*health));
  FILE *uptime = fopen("/proc/uptime", "r");
  if (uptime) {
    double seconds = 0;
    if (fscanf(uptime, "%lf", &seconds) == 1)
      health->uptime_seconds = (long)seconds;
    fclose(uptime);
  }

  const char *thermal_path = getenv("CONFIGD_THERMAL_PATH");
  if (!thermal_path || !*thermal_path) thermal_path = "/sys/class/thermal";
  DIR *directory = opendir(thermal_path);
  if (directory) {
    struct dirent *entry;
    while ((entry = readdir(directory)) &&
           health->temp_count < METRICS_MAX_TEMPS) {
      if (strncmp(entry->d_name, "thermal_zone", 12) != 0) continue;
      char path[256];
      if (snprintf(path, sizeof(path), "%s/%s/temp", thermal_path,
                   entry->d_name) >= (int)sizeof(path)) continue;
      FILE *temperature = fopen(path, "r");
      if (!temperature) continue;
      long milli = 0;
      if (fscanf(temperature, "%ld", &milli) == 1) {
        int index = health->temp_count++;
        health->temps_celsius[index] = (double)milli / 1000.0;
        snprintf(health->temp_labels[index], sizeof(health->temp_labels[index]),
                 "%.31s", entry->d_name);
      }
      fclose(temperature);
    }
    closedir(directory);
  }
  if (hardware.poe_available) health->poe_available = 1;
}

static void detect_discontinuity(struct portstats_snapshot *snapshot) {
  long tick = snapshot->generated_unix;
  int reset = !g_have_prev;
  if (g_have_prev) {
    if (snapshot->timestamp < g_prev.timestamp) reset = 1;
    for (int i = 0; !reset && i < PORTSTATS_MAX_PORTS; i++) {
      if (snapshot->ports[i].present && g_prev.ports[i].present &&
          (snapshot->ports[i].rx_octets < g_prev.ports[i].rx_octets ||
           snapshot->ports[i].tx_octets < g_prev.ports[i].tx_octets))
        reset = 1;
    }
  }
  snapshot->discontinuity_ticks = reset ? tick : g_prev.discontinuity_ticks;
}

static int refresh_snapshot(bool require_valid, char *error, size_t error_size) {
  struct portstats_snapshot snapshot;
  if (portstats_read(&snapshot) != 0 || !snapshot.valid || snapshot.count <= 0) {
    g_snap.valid = 0;
    if (require_valid && error && error_size)
      snprintf(error, error_size,
               "switch port counter source is unavailable or returned no ports");
    return -EIO;
  }
  telemetry_enrich_port_status(&snapshot);
  collect_health(&g_health);
  if (g_health.poe_available) {
    for (int i = 0; i < PORTSTATS_MAX_PORTS; i++)
      if (snapshot.ports[i].present && snapshot.ports[i].poe_present)
        g_health.poe_power_watts += snapshot.ports[i].poe_power_watts;
  }
  detect_discontinuity(&snapshot);
  snapshot.ttl_seconds = telemetry_interval_seconds() * 3;
  g_prev = snapshot;
  g_have_prev = 1;
  g_snap = snapshot;
  if (portstats_write_file(&g_snap) != 0) {
    if (require_valid && error && error_size)
      snprintf(error, error_size, "unable to publish the SNMP port snapshot");
    return -EIO;
  }
  return 0;
}

int telemetry_apply(struct json_object *config, const char *bind_addr,
                    struct apply_result *result) {
  struct json_object *telemetry = member(config, "telemetry");
  struct json_object *prometheus = member(telemetry, "prometheus");
  struct json_object *snmp = member(telemetry, "snmp");
  bool prometheus_enabled = bool_member(prometheus, "enabled", false);
  bool prometheus_management_only =
    bool_member(prometheus, "management_only", true);
  int prometheus_port = int_member(prometheus, "port",
                                   TELEMETRY_PROM_DEFAULT_PORT);
  bool snmp_enabled = bool_member(snmp, "enabled", false);

  if (dry_run) {
    if (prometheus_enabled || metrics_server_running()) apply_result_applied(result);
    if (snmp_enabled) apply_result_applied(result);
    return 0;
  }

  if (prometheus_enabled) {
    char error[160] = {0};
    const char *device = prometheus_management_only ? management_interface() : NULL;
    if (metrics_server_start_bound(NULL, device, prometheus_port,
                                   error, sizeof(error)) != 0) {
      apply_result_fail(result, "Prometheus listener: %s",
                        error[0] ? error : "unable to bind");
      return -EIO;
    }
    apply_result_applied(result);
  } else if (metrics_server_running()) {
    metrics_server_stop();
    apply_result_applied(result);
  }

  char service_error[160] = {0};
  if (snmp_enabled) {
    char snapshot_error[160] = {0};
    if (refresh_snapshot(true, snapshot_error, sizeof(snapshot_error)) != 0) {
      apply_result_fail(result, "SNMP: %s", snapshot_error);
      return -EIO;
    }
    int env_rc = telemetry_write_snmpd_env(config, bind_addr);
    if (env_rc != 0) {
      apply_result_fail(result, "SNMP: unable to write service environment: %s",
                        strerror(-env_rc));
      return -EIO;
    }
    if (service_action("snmp", "restart", service_error,
                       sizeof(service_error)) != 0) {
      apply_result_fail(result, "SNMP: %s",
                        service_error[0] ? service_error : "service restart failed");
      return -EIO;
    }
    apply_result_applied(result);
  } else {
    if (service_action("snmp", "stop", service_error,
                       sizeof(service_error)) != 0) {
      apply_result_fail(result, "SNMP: %s",
                        service_error[0] ? service_error : "service stop failed");
      return -EIO;
    }
    const char *path = getenv("CONFIGD_SNMPD_ENV");
    if (!path || !*path) path = "/run/postmerkos/snmpd.env";
    if (unlink(path) != 0 && errno != ENOENT) {
      apply_result_fail(result, "SNMP: unable to remove stale environment: %s",
                        strerror(errno));
      return -EIO;
    }
  }
  return 0;
}

void telemetry_tick(void) {
  (void)refresh_snapshot(false, NULL, 0);
}

void telemetry_service_io(void) {
  metrics_server_service(&g_snap, &g_health);
}

/* Build a compact live-telemetry frame from the current in-memory snapshot for
   the WebSocket graph stream. Raw monotonic counters + a device timestamp; the
   client derives rates (and uses its own arrival clock for delta-t, since the
   device clock can be wrong/stepping pre-NTP). Independent of the Prometheus/SNMP
   exporters. Caller owns the returned object. */
/* Friendly name for a thermal zone (e.g. "cpu-thermal", "poe-thermal") from its
   type file, falling back to the zone label. Read only for the live stream so the
   Prometheus/SNMP temperature labels stay unchanged. */
static void thermal_zone_name(const char *label, char *out, size_t out_size) {
  const char *base = getenv("CONFIGD_THERMAL_PATH");
  if (!base || !*base) base = "/sys/class/thermal";
  char path[300];
  snprintf(path, sizeof(path), "%s/%s/type", base, label);
  FILE *f = fopen(path, "r");
  if (f) {
    if (fgets(out, out_size, f)) {
      out[strcspn(out, "\r\n")] = '\0';
      fclose(f);
      if (out[0]) return;
    } else {
      fclose(f);
    }
  }
  snprintf(out, out_size, "%s", label);
}

static void temps_add(struct json_object *temps, const char *name, double c) {
  struct json_object *t = json_object_new_object();
  json_object_object_add(t, "name", json_object_new_string(name));
  json_object_object_add(t, "c", json_object_new_double(c));
  json_object_array_add(temps, t);
}

/* Append hwmon sensors (e.g. the tmp411 board sensor) to the stream temps.
   Read only for the live stream; Prometheus/SNMP temperature labels are unchanged. */
static void append_hwmon_temps(struct json_object *temps) {
  const char *base = getenv("CONFIGD_HWMON_PATH");
  if (!base || !*base) base = "/sys/class/hwmon";
  DIR *dir = opendir(base);
  if (!dir) return;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (strncmp(entry->d_name, "hwmon", 5) != 0) continue;
    char chip[64] = "", path[320];
    snprintf(path, sizeof(path), "%s/%s/name", base, entry->d_name);
    FILE *nf = fopen(path, "r");
    if (nf) { if (fgets(chip, sizeof(chip), nf)) chip[strcspn(chip, "\r\n")] = '\0'; fclose(nf); }
    for (int i = 1; i <= 8; i++) {
      snprintf(path, sizeof(path), "%s/%s/temp%d_input", base, entry->d_name, i);
      FILE *tf = fopen(path, "r");
      if (!tf) continue;
      long milli = 0;
      int ok = fscanf(tf, "%ld", &milli) == 1;
      fclose(tf);
      if (!ok) continue;
      char chan[48] = "", label[128];
      snprintf(path, sizeof(path), "%s/%s/temp%d_label", base, entry->d_name, i);
      FILE *lf = fopen(path, "r");
      if (lf) { if (fgets(chan, sizeof(chan), lf)) chan[strcspn(chan, "\r\n")] = '\0'; fclose(lf); }
      if (chan[0]) snprintf(label, sizeof(label), "%s %s", chip[0] ? chip : "hwmon", chan);
      else snprintf(label, sizeof(label), "%s temp%d", chip[0] ? chip : "hwmon", i);
      temps_add(temps, label, milli / 1000.0);
    }
  }
  closedir(dir);
}

struct json_object *telemetry_stream_json(void) {
  struct json_object *data = json_object_new_object();
  json_object_object_add(data, "ts", json_object_new_int64((int64_t)time(NULL)));
  json_object_object_add(data, "uptime_s",
                         json_object_new_int64(g_health.uptime_seconds));

  /* Per-sensor temperatures with friendly names: [{name, c}, ...]. Sources:
     kernel thermal zones, hwmon chips, and the pd690xx PoE controller(s). */
  struct json_object *temps = json_object_new_array();
  for (int i = 0; i < g_health.temp_count && i < METRICS_MAX_TEMPS; i++) {
    char name[64];
    thermal_zone_name(g_health.temp_labels[i], name, sizeof(name));
    temps_add(temps, name, g_health.temps_celsius[i]);
  }
  append_hwmon_temps(temps);
  if (g_health.poe_available) {
    int controllers = pd690xx_pres_count(&pd690xx);
    float *junction = get_temp(&pd690xx);
    if (junction) {
      for (int i = 0; i < controllers; i++) {
        char name[24];
        if (controllers > 1) snprintf(name, sizeof(name), "poe-%d", i + 1);
        else snprintf(name, sizeof(name), "poe");
        temps_add(temps, name, junction[i]);
      }
      free(junction);
    }
  }
  json_object_object_add(data, "temps", temps);

  if (g_health.poe_available)
    json_object_object_add(data, "poe_w",
                           json_object_new_double(g_health.poe_power_watts));

  /* CPU load average (1/5/15 min) from /proc/loadavg. */
  struct json_object *load = json_object_new_array();
  const char *loadavg = getenv("CONFIGD_LOADAVG_FILE");
  if (!loadavg || !*loadavg) loadavg = "/proc/loadavg";
  FILE *lf = fopen(loadavg, "r");
  if (lf) {
    double a = 0, b = 0, c = 0;
    if (fscanf(lf, "%lf %lf %lf", &a, &b, &c) >= 1) {
      json_object_array_add(load, json_object_new_double(a));
      json_object_array_add(load, json_object_new_double(b));
      json_object_array_add(load, json_object_new_double(c));
    }
    fclose(lf);
  }
  json_object_object_add(data, "load", load);

  /* Memory from /proc/meminfo (kB); the client derives used %. */
  const char *meminfo = getenv("CONFIGD_MEMINFO_FILE");
  if (!meminfo || !*meminfo) meminfo = "/proc/meminfo";
  FILE *mf = fopen(meminfo, "r");
  if (mf) {
    char line[128]; long total = -1, avail = -1; long value;
    while ((total < 0 || avail < 0) && fgets(line, sizeof(line), mf)) {
      if (sscanf(line, "MemTotal: %ld kB", &value) == 1) total = value;
      else if (sscanf(line, "MemAvailable: %ld kB", &value) == 1) avail = value;
    }
    fclose(mf);
    if (total > 0) {
      json_object_object_add(data, "mem_total_kb", json_object_new_int64(total));
      if (avail >= 0) json_object_object_add(data, "mem_avail_kb", json_object_new_int64(avail));
    }
  }

  struct json_object *ports = json_object_new_array();
  uint64_t total_rx = 0, total_tx = 0;
  for (int i = 0; i < g_snap.count && i < PORTSTATS_MAX_PORTS; i++) {
    const struct port_counters *p = &g_snap.ports[i];
    if (!p->present) continue;
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "i", json_object_new_int(p->port));
    json_object_object_add(o, "rxB", json_object_new_int64((int64_t)p->rx_octets));
    json_object_object_add(o, "txB", json_object_new_int64((int64_t)p->tx_octets));
    json_object_object_add(o, "rxP", json_object_new_int64((int64_t)p->rx_packets));
    json_object_object_add(o, "txP", json_object_new_int64((int64_t)p->tx_packets));
    json_object_object_add(o, "up", json_object_new_int(p->oper == PORT_LINK_UP ? 1 : 0));
    json_object_object_add(o, "spd", json_object_new_int(p->speed_mbps));
    if (p->poe_present)
      json_object_object_add(o, "poeW", json_object_new_double(p->poe_power_watts));
    json_object_array_add(ports, o);
    total_rx += p->rx_octets;
    total_tx += p->tx_octets;
  }
  json_object_object_add(data, "ports", ports);
  struct json_object *total = json_object_new_object();
  json_object_object_add(total, "rxB", json_object_new_int64((int64_t)total_rx));
  json_object_object_add(total, "txB", json_object_new_int64((int64_t)total_tx));
  json_object_object_add(data, "total", total);
  return data;
}

void telemetry_shutdown(void) {
  metrics_server_stop();
}
