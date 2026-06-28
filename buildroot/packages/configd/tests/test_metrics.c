#include "../metrics.h"
#include "../portstats.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  struct portstats_snapshot snap;
  memset(&snap, 0, sizeof(snap));
  snap.valid = 1;
  snap.count = 1;
  snap.ports[0].present = 1;
  snap.ports[0].port = 1;
  snap.ports[0].rx_octets = 3613875132ULL;
  snap.ports[0].tx_octets = 1020153062ULL;
  snap.ports[0].rx_packets = 28117844ULL;
  snap.ports[0].tx_packets = 13943591ULL;
  snap.ports[0].oper = PORT_LINK_UP;
  snap.ports[0].speed_mbps = 1000;

  struct device_health health;
  memset(&health, 0, sizeof(health));
  health.uptime_seconds = 123456;
  health.temp_count = 1;
  health.temps_celsius[0] = 54.0;
  snprintf(health.temp_labels[0], sizeof(health.temp_labels[0]), "thermal_zone0");

  char buf[65536];
  int len = metrics_render(buf, sizeof(buf), &snap, &health);
  assert(len > 0);

  assert(strstr(buf, "# HELP postmerkos_scrape_success"));
  assert(strstr(buf, "# TYPE postmerkos_scrape_success gauge"));
  assert(strstr(buf, "postmerkos_scrape_success 1"));
  assert(strstr(buf, "# TYPE postmerkos_port_receive_bytes_total counter"));
  assert(strstr(buf, "postmerkos_port_receive_bytes_total{port=\"1\",ifname=\"port1\"} 3613875132"));
  assert(strstr(buf, "postmerkos_port_transmit_bytes_total{port=\"1\",ifname=\"port1\"} 1020153062"));
  assert(strstr(buf, "postmerkos_port_receive_packets_total{port=\"1\",ifname=\"port1\"} 28117844"));
  assert(strstr(buf, "postmerkos_port_up{port=\"1\",ifname=\"port1\"} 1"));
  assert(strstr(buf, "postmerkos_port_speed_mbps{port=\"1\",ifname=\"port1\"} 1000"));
  assert(strstr(buf, "postmerkos_uptime_seconds 123456"));
  assert(strstr(buf, "postmerkos_temperature_celsius{sensor=\"thermal_zone0\"} 54"));

  /* HELP/TYPE for a counter family appears exactly once */
  const char *first = strstr(buf, "# TYPE postmerkos_port_receive_bytes_total");
  assert(first);
  assert(strstr(first + 1, "# TYPE postmerkos_port_receive_bytes_total") == NULL);

  /* invalid snapshot: scrape_success 0, no per-port series, still valid text */
  struct portstats_snapshot bad;
  memset(&bad, 0, sizeof(bad));
  bad.valid = 0;
  int len2 = metrics_render(buf, sizeof(buf), &bad, &health);
  assert(len2 > 0);
  assert(strstr(buf, "postmerkos_scrape_success 0"));
  assert(strstr(buf, "postmerkos_port_receive_bytes_total{") == NULL);
  assert(strstr(buf, "postmerkos_uptime_seconds 123456"));  /* health still emitted */

  /* tiny buffer: overflow returns -1 */
  char tiny[16];
  assert(metrics_render(tiny, sizeof(tiny), &snap, &health) == -1);

  puts("metrics render tests passed");
  return 0;
}
