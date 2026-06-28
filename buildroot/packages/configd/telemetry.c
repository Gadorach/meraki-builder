#include "telemetry.h"

#include "metrics.h"
#include "portstats.h"
#include "hardware.h"
#include "service_ops.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libpostmerkos.h>   /* get_time, click/read helpers, get_field_copy */
#include <libpd690xx.h>      /* port_power, get_temp */

/* External hardware/pd690xx config (defined in main/hardware.c) */
extern struct hardware_info hardware;
extern struct pd690xx_cfg pd690xx;

/* ---- shared state (single-threaded main loop: no locking needed) ---- */
static struct portstats_snapshot g_snap;
static struct device_health g_health;
static struct portstats_snapshot g_prev;   /* for discontinuity detection */
static int g_have_prev;

/* ---- small json helpers (mirroring service_ops.c idioms) ---- */
static struct json_object *member(struct json_object *o, const char *k) {
  struct json_object *v = NULL;
  if (!o || !json_object_is_type(o, json_type_object) ||
      !json_object_object_get_ex(o, k, &v)) return NULL;
  return v;
}
static bool bool_member(struct json_object *o, const char *k, bool fb) {
  struct json_object *v = member(o, k);
  return v && json_object_is_type(v, json_type_boolean) ? json_object_get_boolean(v) : fb;
}
static int int_member(struct json_object *o, const char *k, int fb) {
  struct json_object *v = member(o, k);
  return v && json_object_is_type(v, json_type_int) ? json_object_get_int(v) : fb;
}
static const char *str_member(struct json_object *o, const char *k, const char *fb) {
  struct json_object *v = member(o, k);
  return v && json_object_is_type(v, json_type_string) ? json_object_get_string(v) : fb;
}

static int bad(char *error, size_t n, const char *fmt, ...) {
  if (error && n) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(error, n, fmt, ap);
    va_end(ap);
  }
  return -EINVAL;
}

struct json_object *telemetry_default_config(void) {
  struct json_object *t = json_object_new_object();
  struct json_object *snmp = json_object_new_object();
  json_object_object_add(snmp, "enabled", json_object_new_boolean(0));
  json_object_object_add(snmp, "community", json_object_new_string(""));
  json_object_object_add(snmp, "location", json_object_new_string(""));
  json_object_object_add(snmp, "contact", json_object_new_string(""));
  json_object_object_add(t, "snmp", snmp);
  struct json_object *prom = json_object_new_object();
  json_object_object_add(prom, "enabled", json_object_new_boolean(0));
  json_object_object_add(prom, "port", json_object_new_int(TELEMETRY_PROM_DEFAULT_PORT));
  json_object_object_add(t, "prometheus", prom);
  return t;
}

static int key_allowed(const char *k, const char *const *allowed, size_t n) {
  for (size_t i = 0; i < n; i++) if (!strcmp(k, allowed[i])) return 1;
  return 0;
}
static int reject_unknown(struct json_object *o, const char *const *allowed,
                          size_t n, const char *path, char *err, size_t errn) {
  json_object_object_foreach(o, k, v) {
    (void)v;
    if (!key_allowed(k, allowed, n)) return bad(err, errn, "%s.%s is not supported", path, k);
  }
  return 0;
}

int telemetry_validate(struct json_object *config, char *error, size_t error_size) {
  struct json_object *t = member(config, "telemetry");
  if (!t) return 0;                       /* absent is valid */
  if (!json_object_is_type(t, json_type_object))
    return bad(error, error_size, "telemetry must be an object");
  const char *keys[] = {"snmp", "prometheus"};
  if (reject_unknown(t, keys, 2, "telemetry", error, error_size) != 0) return -EINVAL;

  struct json_object *snmp = member(t, "snmp");
  if (snmp) {
    if (!json_object_is_type(snmp, json_type_object))
      return bad(error, error_size, "telemetry.snmp must be an object");
    const char *sk[] = {"enabled", "community", "location", "contact"};
    if (reject_unknown(snmp, sk, 4, "telemetry.snmp", error, error_size) != 0) return -EINVAL;
    bool enabled = bool_member(snmp, "enabled", false);
    const char *community = str_member(snmp, "community", "");
    /* community required when enabled; printable, no whitespace/control, bounded */
    if (enabled && (!community || !*community))
      return bad(error, error_size, "telemetry.snmp.community is required when SNMP is enabled");
    if (strlen(community) > 64)
      return bad(error, error_size, "telemetry.snmp.community is too long");
    for (const unsigned char *p = (const unsigned char *)community; *p; p++)
      if (*p < 0x21 || *p == 0x7f)
        return bad(error, error_size, "telemetry.snmp.community contains invalid characters");
    if (strlen(str_member(snmp, "location", "")) > 256)
      return bad(error, error_size, "telemetry.snmp.location is too long");
    if (strlen(str_member(snmp, "contact", "")) > 256)
      return bad(error, error_size, "telemetry.snmp.contact is too long");
  }

  struct json_object *prom = member(t, "prometheus");
  if (prom) {
    if (!json_object_is_type(prom, json_type_object))
      return bad(error, error_size, "telemetry.prometheus must be an object");
    const char *pk[] = {"enabled", "port"};
    if (reject_unknown(prom, pk, 2, "telemetry.prometheus", error, error_size) != 0) return -EINVAL;
    struct json_object *pv = member(prom, "port");
    if (pv) {
      if (!json_object_is_type(pv, json_type_int))
        return bad(error, error_size, "telemetry.prometheus.port must be an integer");
      int port = json_object_get_int(pv);
      if (port < 1 || port > 65535)
        return bad(error, error_size, "telemetry.prometheus.port must be between 1 and 65535");
      /* static denylist (spec §9.3): SNMP ports + UI/web port 80 */
      if (port == 161 || port == 162 || port == 80)
        return bad(error, error_size, "telemetry.prometheus.port %d is reserved", port);
    }
  }
  return 0;
}

void telemetry_apply(struct json_object *config, const char *bind_addr) {
  struct json_object *t = member(config, "telemetry");
  struct json_object *prom = member(t, "prometheus");
  bool enabled = bool_member(prom, "enabled", false);
  int port = int_member(prom, "port", TELEMETRY_PROM_DEFAULT_PORT);

  if (enabled) {
    /* restart on (re)apply to pick up a port change.
     * Bind 0.0.0.0: the device's L3 management IP lives in the Click/brain
     * datapath, not on a Linux interface, so it is not bindable here (same
     * reason the websocket and mini_snmpd bind all interfaces). bind_addr is
     * retained only for the informational SNMP_BIND in the snmpd env. */
    metrics_server_stop();
    if (metrics_server_start(NULL, port) != 0)
      fprintf(stderr, "%s telemetry: metrics listener bind failed on 0.0.0.0:%d\n",
              get_time(), port);
    else
      fprintf(stderr, "%s telemetry: metrics listener started on 0.0.0.0:%d\n",
              get_time(), port);
  } else if (metrics_server_running()) {
    metrics_server_stop();
    fprintf(stderr, "%s telemetry: metrics listener stopped\n", get_time());
  }

  /* --- SNMP --- */
  struct json_object *snmp = member(t, "snmp");
  bool snmp_enabled = bool_member(snmp, "enabled", false);
  char serr[128] = {0};
  if (snmp_enabled) {
    telemetry_write_snmpd_env(config, bind_addr);
    if (service_action("snmp", "restart", serr, sizeof(serr)) != 0)
      fprintf(stderr, "%s telemetry: snmp start failed: %s\n", get_time(), serr);
  } else {
    service_action("snmp", "stop", serr, sizeof(serr));
  }
}

int telemetry_interval_seconds(void) {
  const char *e = getenv("CONFIGD_PORTSTATS_INTERVAL");
  int v = e ? atoi(e) : 5;
  return v >= 1 ? v : 5;
}

int telemetry_server_fd(void) { return metrics_server_fd(); }

/* ---- snmpd.env writer ---- */

static void sq(FILE *f, const char *key, const char *val) {
  /* single-quote, turning ' into '\'' */
  fprintf(f, "%s='", key);
  for (const char *p = val ? val : ""; *p; p++) {
    if (*p == '\'') fputs("'\\''", f);
    else fputc(*p, f);
  }
  fputs("'\n", f);
}

int telemetry_write_snmpd_env(struct json_object *config, const char *bind_addr) {
  struct json_object *snmp = member(member(config, "telemetry"), "snmp");
  const char *community = str_member(snmp, "community", "");
  const char *location = str_member(snmp, "location", "");
  const char *contact = str_member(snmp, "contact", "");

  const char *path = getenv("CONFIGD_SNMPD_ENV");
  if (!path || !*path) path = "/run/postmerkos/snmpd.env";
  char tmp[512];
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;

  FILE *f = fopen(tmp, "w");
  if (!f) return -1;
  sq(f, "SNMP_COMMUNITY", community);
  sq(f, "SNMP_LOCATION", location);
  sq(f, "SNMP_CONTACT", contact);
  sq(f, "SNMP_BIND", bind_addr ? bind_addr : "");
  if (fflush(f) != 0) { fclose(f); unlink(tmp); return -1; }
  int fd = fileno(f); if (fd >= 0) fsync(fd);
  if (fclose(f) != 0) { unlink(tmp); return -1; }
  if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
  return 0;
}

/* ---- telemetry_tick implementation ---- */

static enum port_link_state phy_admin_state(unsigned int port) {
  char line[512], mode[32];
  if (read_switch_port_table("dump_port_phy_cfgs", port, line, sizeof(line)) != 0 ||
      get_field_copy(line, 2, mode, sizeof(mode)) != 0)
    return PORT_LINK_UNKNOWN;
  return strcmp(mode, "off") == 0 ? PORT_LINK_DOWN : PORT_LINK_UP;
}

static void enrich_port_status(struct portstats_snapshot *snap) {
  /* Use the SAME handler status.c reads for link state: PORTS_FILE
   * (/click/switch_port_table/dump_pports), field 2 = established, field 3 =
   * speed Mbps. dump_lports is STP state, not link state — wrong source. */
  const char *ports_path = getenv("CONFIGD_PORTS_FILE");
  if (!ports_path || !*ports_path) ports_path = PORTS_FILE;
  FILE *f = fopen(ports_path, "r");
  if (!f) return;
  char line[512];
  unsigned int port = 0;
  bool header = true;
  while (fgets(line, sizeof(line), f)) {
    if (header) { header = false; continue; }
    port++;
    if (port < 1 || port > PORTSTATS_MAX_PORTS) continue;
    struct port_counters *p = &snap->ports[port - 1];
    char est[32] = "0", spd[32] = "0";
    if (get_field_copy(line, 2, est, sizeof(est)) == 0 &&
        get_field_copy(line, 3, spd, sizeof(spd)) == 0) {
      p->oper = atoi(est) != 0 ? PORT_LINK_UP : PORT_LINK_DOWN;
      p->speed_mbps = atoi(spd);
    }
    p->admin = phy_admin_state(port);
    if (hardware.poe_available) {
      float w = port_power(&pd690xx, (int)port);
      if (w >= 0) { p->poe_present = 1; p->poe_power_watts = (double)w; }
    }
  }
  fclose(f);
}

static void collect_health(struct device_health *h) {
  memset(h, 0, sizeof(*h));
  /* uptime */
  FILE *u = fopen("/proc/uptime", "r");
  if (u) { double up = 0; if (fscanf(u, "%lf", &up) == 1) h->uptime_seconds = (long)up; fclose(u); }
  /* thermal zones */
  const char *tp = getenv("CONFIGD_THERMAL_PATH");
  if (!tp || !*tp) tp = "/sys/class/thermal";
  DIR *d = opendir(tp);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) && h->temp_count < METRICS_MAX_TEMPS) {
      if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
      char path[256];
      if (snprintf(path, sizeof(path), "%s/%s/temp", tp, e->d_name) >= (int)sizeof(path)) continue;
      FILE *tf = fopen(path, "r");
      if (!tf) continue;
      long milli = 0;
      if (fscanf(tf, "%ld", &milli) == 1) {
        h->temps_celsius[h->temp_count] = (double)milli / 1000.0;
        snprintf(h->temp_labels[h->temp_count], sizeof(h->temp_labels[0]), "%.31s", e->d_name);
        h->temp_count++;
      }
      fclose(tf);
    }
    closedir(d);
  }
  if (hardware.poe_available) {
    h->poe_available = 1;
    /* aggregate PoE: sum per-port draw (budget left as 0 if unavailable) */
    /* per-port draw summed in telemetry_tick after enrichment */
  }
}

static void detect_discontinuity(struct portstats_snapshot *snap) {
  long tick = snap->generated_unix;  /* simple uptime-like tick */
  int reset = 0;
  if (!g_have_prev) {
    reset = 1;  /* conservative: see spec §5.5 caveat */
  } else {
    if (snap->timestamp < g_prev.timestamp) reset = 1;
    for (int i = 0; !reset && i < PORTSTATS_MAX_PORTS; i++) {
      if (snap->ports[i].present && g_prev.ports[i].present &&
          (snap->ports[i].rx_octets < g_prev.ports[i].rx_octets ||
           snap->ports[i].tx_octets < g_prev.ports[i].tx_octets))
        reset = 1;
    }
  }
  if (reset) snap->discontinuity_ticks = tick;
  else snap->discontinuity_ticks = g_prev.discontinuity_ticks;
}

void telemetry_tick(void) {
  struct portstats_snapshot snap;
  if (portstats_read(&snap) != 0) {
    /* read/decode failed: keep last good snapshot for SNMP, mark invalid for /metrics */
    g_snap.valid = 0;
    metrics_server_service(&g_snap, &g_health);
    return;
  }
  enrich_port_status(&snap);
  collect_health(&g_health);
  /* aggregate PoE = sum of per-port draw */
  if (g_health.poe_available) {
    double total = 0;
    for (int i = 0; i < PORTSTATS_MAX_PORTS; i++)
      if (snap.ports[i].present && snap.ports[i].poe_present)
        total += snap.ports[i].poe_power_watts;
    g_health.poe_power_watts = total;
  }
  detect_discontinuity(&snap);
  snap.ttl_seconds = telemetry_interval_seconds() * 3;

  g_prev = snap;
  g_have_prev = 1;
  g_snap = snap;

  portstats_write_file(&g_snap);                 /* SNMP feed (Plan B consumes) */
  metrics_server_service(&g_snap, &g_health);    /* serve any pending scrape */
}
