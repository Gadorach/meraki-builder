#include "metrics.h"
#include "portstats.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TEST_PORT_BASE 19100
#define MAX_PORT_RETRIES 3

static int try_start_server(int *port_out) {
  for (int i = 0; i < MAX_PORT_RETRIES; i++) {
    int port = TEST_PORT_BASE + i;
    if (metrics_server_start("127.0.0.1", port) == 0) {
      *port_out = port;
      return 0;
    }
  }
  return -1;
}

static int connect_to_server(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void send_all(int fd, const char *data) {
  size_t len = strlen(data);
  size_t off = 0;
  while (off < len) {
    ssize_t w = send(fd, data + off, len - off, 0);
    if (w <= 0) { if (errno == EINTR) continue; break; }
    off += (size_t)w;
  }
}

static int recv_response(int fd, char *buf, size_t cap) {
  size_t used = 0;
  while (used < cap - 1) {
    ssize_t r = recv(fd, buf + used, cap - 1 - used, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) break;
    used += (size_t)r;
  }
  buf[used] = '\0';
  return (int)used;
}

static void test_valid_metrics_request(int port,
                                       const struct portstats_snapshot *snap,
                                       const struct device_health *health) {
  int cfd = connect_to_server(port);
  if (cfd < 0) { fprintf(stderr, "Failed to connect for /metrics test\n"); exit(1); }

  send_all(cfd, "GET /metrics HTTP/1.0\r\n\r\n");
  metrics_server_service(snap, health);

  char resp[64 * 1024];
  int rlen = recv_response(cfd, resp, sizeof(resp));
  close(cfd);

  if (rlen <= 0) { fprintf(stderr, "/metrics: no response\n"); exit(1); }

  if (strncmp(resp, "HTTP/1.0 200", 12) != 0) {
    fprintf(stderr, "/metrics: expected HTTP/1.0 200, got: %.20s\n", resp);
    exit(1);
  }
  if (!strstr(resp, "Content-Type: text/plain; version=0.0.4")) {
    fprintf(stderr, "/metrics: missing Content-Type header\n");
    exit(1);
  }
  if (!strstr(resp, "Content-Length:")) {
    fprintf(stderr, "/metrics: missing Content-Length header\n");
    exit(1);
  }
  if (!strstr(resp, "postmerkos_scrape_success 1")) {
    fprintf(stderr, "/metrics: missing scrape_success=1\n");
    exit(1);
  }
  if (!strstr(resp, "postmerkos_port_receive_bytes_total{port=\"1\"")) {
    fprintf(stderr, "/metrics: missing port 1 metrics\n");
    exit(1);
  }
}

static void test_invalid_path(int port,
                              const struct portstats_snapshot *snap,
                              const struct device_health *health) {
  int cfd = connect_to_server(port);
  if (cfd < 0) { fprintf(stderr, "Failed to connect for /metricsX test\n"); exit(1); }

  send_all(cfd, "GET /metricsX HTTP/1.0\r\n\r\n");
  metrics_server_service(snap, health);

  char resp[1024];
  int rlen = recv_response(cfd, resp, sizeof(resp));
  close(cfd);

  if (rlen <= 0) { fprintf(stderr, "/metricsX: no response\n"); exit(1); }

  if (strncmp(resp, "HTTP/1.0 404", 12) != 0) {
    fprintf(stderr, "/metricsX: expected HTTP/1.0 404, got: %.20s\n", resp);
    exit(1);
  }
}

static void test_root_path(int port,
                           const struct portstats_snapshot *snap,
                           const struct device_health *health) {
  int cfd = connect_to_server(port);
  if (cfd < 0) { fprintf(stderr, "Failed to connect for / test\n"); exit(1); }

  send_all(cfd, "GET / HTTP/1.0\r\n\r\n");
  metrics_server_service(snap, health);

  char resp[1024];
  int rlen = recv_response(cfd, resp, sizeof(resp));
  close(cfd);

  if (rlen <= 0) { fprintf(stderr, "/: no response\n"); exit(1); }

  if (strncmp(resp, "HTTP/1.0 404", 12) != 0) {
    fprintf(stderr, "/: expected HTTP/1.0 404, got: %.20s\n", resp);
    exit(1);
  }
}

int main(void) {
  struct portstats_snapshot snap;
  memset(&snap, 0, sizeof(snap));
  snap.valid = 1;
  snap.timestamp = 1234567890;
  snap.discontinuity_ticks = 100;
  snap.ports[0].present = 1;
  snap.ports[0].port = 1;
  snap.ports[0].rx_octets = 12345678;
  snap.ports[0].tx_octets = 87654321;
  snap.ports[0].rx_packets = 1000;
  snap.ports[0].tx_packets = 2000;
  snap.ports[0].oper = PORT_LINK_UP;
  snap.ports[0].speed_mbps = 1000;

  struct device_health health;
  memset(&health, 0, sizeof(health));
  health.uptime_seconds = 3600;

  int port;
  if (try_start_server(&port) != 0) {
    fprintf(stderr, "Failed to start metrics server on any test port\n");
    return 1;
  }

  test_valid_metrics_request(port, &snap, &health);
  test_invalid_path(port, &snap, &health);
  test_root_path(port, &snap, &health);

  metrics_server_stop();

  printf("metrics server tests passed\n");
  return 0;
}
