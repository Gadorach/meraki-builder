#include "metrics.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct sink {
  char *buf;
  size_t cap;
  size_t len;
  int overflow;
};

static void emit(struct sink *s, const char *fmt, ...) {
  if (s->overflow) return;
  va_list ap;
  va_start(ap, fmt);
  int avail = (int)(s->cap - s->len);
  int wrote = vsnprintf(s->buf + s->len, (size_t)avail, fmt, ap);
  va_end(ap);
  if (wrote < 0 || wrote >= avail) { s->overflow = 1; return; }
  s->len += (size_t)wrote;
}

int metrics_render(char *buf, size_t n,
                   const struct portstats_snapshot *snap,
                   const struct device_health *health) {
  struct sink s = { buf, n, 0, 0 };
  int ok = snap && snap->valid;

  emit(&s, "# HELP postmerkos_scrape_success Whether the portstats scrape succeeded.\n");
  emit(&s, "# TYPE postmerkos_scrape_success gauge\n");
  emit(&s, "postmerkos_scrape_success %d\n", ok ? 1 : 0);

  if (ok) {
    emit(&s, "# HELP postmerkos_portstats_timestamp_seconds Click source timestamp.\n");
    emit(&s, "# TYPE postmerkos_portstats_timestamp_seconds gauge\n");
    emit(&s, "postmerkos_portstats_timestamp_seconds %ld\n", snap->timestamp);
    emit(&s, "# HELP postmerkos_portstats_discontinuity_seconds Last counter-reset tick.\n");
    emit(&s, "# TYPE postmerkos_portstats_discontinuity_seconds gauge\n");
    emit(&s, "postmerkos_portstats_discontinuity_seconds %ld\n", snap->discontinuity_ticks);

    emit(&s, "# HELP postmerkos_port_receive_bytes_total Received octets per port.\n");
    emit(&s, "# TYPE postmerkos_port_receive_bytes_total counter\n");
    for (int i = 0; i < PORTSTATS_MAX_PORTS; i++) {
      const struct port_counters *p = &snap->ports[i];
      if (!p->present) continue;
      emit(&s, "postmerkos_port_receive_bytes_total{port=\"%d\",ifname=\"port%d\"} %llu\n",
           p->port, p->port, (unsigned long long)p->rx_octets);
    }
    emit(&s, "# HELP postmerkos_port_transmit_bytes_total Transmitted octets per port.\n");
    emit(&s, "# TYPE postmerkos_port_transmit_bytes_total counter\n");
    for (int i = 0; i < PORTSTATS_MAX_PORTS; i++) {
      const struct port_counters *p = &snap->ports[i];
      if (!p->present) continue;
      emit(&s, "postmerkos_port_transmit_bytes_total{port=\"%d\",ifname=\"port%d\"} %llu\n",
           p->port, p->port, (unsigned long long)p->tx_octets);
    }
    emit(&s, "# HELP postmerkos_port_receive_packets_total Received packets per port.\n");
    emit(&s, "# TYPE postmerkos_port_receive_packets_total counter\n");
    for (int i = 0; i < PORTSTATS_MAX_PORTS; i++) {
      const struct port_counters *p = &snap->ports[i];
      if (!p->present) continue;
      emit(&s, "postmerkos_port_receive_packets_total{port=\"%d\",ifname=\"port%d\"} %llu\n",
           p->port, p->port, (unsigned long long)p->rx_packets);
    }
    emit(&s, "# HELP postmerkos_port_transmit_packets_total Transmitted packets per port.\n");
    emit(&s, "# TYPE postmerkos_port_transmit_packets_total counter\n");
    for (int i = 0; i < PORTSTATS_MAX_PORTS; i++) {
      const struct port_counters *p = &snap->ports[i];
      if (!p->present) continue;
      emit(&s, "postmerkos_port_transmit_packets_total{port=\"%d\",ifname=\"port%d\"} %llu\n",
           p->port, p->port, (unsigned long long)p->tx_packets);
    }
    emit(&s, "# HELP postmerkos_port_up Operational link state per port.\n");
    emit(&s, "# TYPE postmerkos_port_up gauge\n");
    for (int i = 0; i < PORTSTATS_MAX_PORTS; i++) {
      const struct port_counters *p = &snap->ports[i];
      if (!p->present) continue;
      emit(&s, "postmerkos_port_up{port=\"%d\",ifname=\"port%d\"} %d\n",
           p->port, p->port, p->oper == PORT_LINK_UP ? 1 : 0);
    }
    emit(&s, "# HELP postmerkos_port_speed_mbps Negotiated link speed per port.\n");
    emit(&s, "# TYPE postmerkos_port_speed_mbps gauge\n");
    for (int i = 0; i < PORTSTATS_MAX_PORTS; i++) {
      const struct port_counters *p = &snap->ports[i];
      if (!p->present) continue;
      emit(&s, "postmerkos_port_speed_mbps{port=\"%d\",ifname=\"port%d\"} %d\n",
           p->port, p->port, p->speed_mbps);
    }
    /* per-port PoE, only for ports with a valid reading */
    int any_poe = 0;
    for (int i = 0; i < PORTSTATS_MAX_PORTS; i++)
      if (snap->ports[i].present && snap->ports[i].poe_present) { any_poe = 1; break; }
    if (any_poe) {
      emit(&s, "# HELP postmerkos_poe_port_power_watts Per-port PoE power draw.\n");
      emit(&s, "# TYPE postmerkos_poe_port_power_watts gauge\n");
      for (int i = 0; i < PORTSTATS_MAX_PORTS; i++) {
        const struct port_counters *p = &snap->ports[i];
        if (!p->present || !p->poe_present) continue;
        emit(&s, "postmerkos_poe_port_power_watts{port=\"%d\",ifname=\"port%d\"} %.3f\n",
             p->port, p->port, p->poe_power_watts);
      }
    }
  }

  if (health) {
    emit(&s, "# HELP postmerkos_uptime_seconds System uptime.\n");
    emit(&s, "# TYPE postmerkos_uptime_seconds gauge\n");
    emit(&s, "postmerkos_uptime_seconds %ld\n", health->uptime_seconds);
    if (health->temp_count > 0) {
      emit(&s, "# HELP postmerkos_temperature_celsius Temperature sensors.\n");
      emit(&s, "# TYPE postmerkos_temperature_celsius gauge\n");
      for (int i = 0; i < health->temp_count && i < METRICS_MAX_TEMPS; i++)
        emit(&s, "postmerkos_temperature_celsius{sensor=\"%s\"} %g\n",
             health->temp_labels[i], health->temps_celsius[i]);
    }
    if (health->poe_available) {
      emit(&s, "# HELP postmerkos_poe_power_watts Aggregate PoE power consumption.\n");
      emit(&s, "# TYPE postmerkos_poe_power_watts gauge\n");
      emit(&s, "postmerkos_poe_power_watts %.3f\n", health->poe_power_watts);
      emit(&s, "# HELP postmerkos_poe_budget_watts Aggregate PoE power budget.\n");
      emit(&s, "# TYPE postmerkos_poe_budget_watts gauge\n");
      emit(&s, "postmerkos_poe_budget_watts %.3f\n", health->poe_budget_watts);
    }
  }

  if (s.overflow) return -1;
  return (int)s.len;
}
