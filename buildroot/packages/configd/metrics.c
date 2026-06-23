#include "metrics.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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

#define METRICS_BODY_MAX (256 * 1024)
#define METRICS_REQLINE_MAX 2048
#define METRICS_CONN_TIMEOUT_MS 2000

static int g_listen_fd = -1;

static long now_monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int metrics_server_fd(void) { return g_listen_fd; }
int metrics_server_running(void) { return g_listen_fd >= 0; }

int metrics_server_start(const char *bind_addr, int port) {
  if (g_listen_fd >= 0) metrics_server_stop();

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (!bind_addr || !*bind_addr) addr.sin_addr.s_addr = htonl(INADDR_ANY);
  else if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) { close(fd); return -1; }

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
  if (listen(fd, 8) != 0) { close(fd); return -1; }

  int flags = fcntl(fd, F_GETFL, 0);
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) { close(fd); return -1; }

  g_listen_fd = fd;
  return 0;
}

void metrics_server_stop(void) {
  if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
}

/* Read the request line (up to CRLF) with a bounded timeout. Returns 0 on
 * success with line NUL-terminated, -1 on error/timeout. */
static int read_request_line(int cfd, char *line, size_t cap) {
  size_t used = 0;
  long deadline_ms = now_monotonic_ms() + METRICS_CONN_TIMEOUT_MS;
  while (used + 1 < cap) {
    long remaining = deadline_ms - now_monotonic_ms();
    if (remaining <= 0) return -1;
    struct pollfd pfd = { cfd, POLLIN, 0 };
    int pr = poll(&pfd, 1, (int)remaining);
    if (pr <= 0) return -1;
    char c;
    ssize_t r = recv(cfd, &c, 1, 0);
    if (r <= 0) return -1;
    if (c == '\n') { line[used] = '\0'; return 0; }
    if (c != '\r') line[used++] = c;
  }
  return -1;
}

static void write_all(int cfd, const char *data, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t w = send(cfd, data + off, len - off, MSG_NOSIGNAL);
    if (w <= 0) { if (errno == EINTR) continue; break; }
    off += (size_t)w;
  }
}

/* Close gracefully so the kernel sends FIN, not RST. We only read the request
 * line, so the client's remaining request headers sit unread in the RX queue;
 * close() with unread data would emit a RST and truncate the response the
 * client is still reading. Half-close our write side, then drain the input
 * with a bounded linger before closing. */
static void lingering_close(int cfd) {
  shutdown(cfd, SHUT_WR);
  long deadline = now_monotonic_ms() + 1000;  /* cap linger at ~1s */
  for (;;) {
    long remaining = deadline - now_monotonic_ms();
    if (remaining <= 0) break;
    struct pollfd pfd = { cfd, POLLIN, 0 };
    if (poll(&pfd, 1, (int)remaining) <= 0) break;
    char buf[512];
    if (recv(cfd, buf, sizeof(buf), 0) <= 0) break;  /* 0 = client closed */
  }
  close(cfd);
}

void metrics_server_service(const struct portstats_snapshot *snap,
                            const struct device_health *health) {
  if (g_listen_fd < 0) return;
  /* Drain pending connections (one render reused across all this cycle). */
  for (;;) {
    int cfd = accept(g_listen_fd, NULL, NULL);
    if (cfd < 0) break;  /* EAGAIN/EWOULDBLOCK: no more pending */

    char line[METRICS_REQLINE_MAX];
    if (read_request_line(cfd, line, sizeof(line)) == 0 &&
        strncmp(line, "GET /metrics", 12) == 0 &&
        (line[12] == ' ' || line[12] == '\0')) {
      /* Safe because this server runs single-threaded in the main loop (no reentrancy). */
      static char body[METRICS_BODY_MAX];
      int blen = metrics_render(body, sizeof(body), snap, health);
      if (blen >= 0) {
        char head[256];
        int hn = snprintf(head, sizeof(head),
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/plain; version=0.0.4\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n", blen);
        write_all(cfd, head, (size_t)hn);
        write_all(cfd, body, (size_t)blen);
      } else {
        const char *e = "HTTP/1.0 500 Internal Server Error\r\n"
                        "Connection: close\r\n\r\n";
        write_all(cfd, e, strlen(e));
      }
    } else {
      const char *nf = "HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\n";
      write_all(cfd, nf, strlen(nf));
    }
    lingering_close(cfd);
  }
}
