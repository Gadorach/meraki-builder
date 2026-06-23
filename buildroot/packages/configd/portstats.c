#include "portstats.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Read a base-128 varint. Returns bytes consumed, or 0 on error
 * (truncated, or more than 10 continuation bytes). */
static size_t read_varint(const unsigned char *b, size_t len, uint64_t *out) {
  uint64_t result = 0;
  size_t i = 0;
  int shift = 0;
  while (i < len) {
    if (i >= 10) return 0;            /* varint too long */
    unsigned char byte = b[i];
    result |= (uint64_t)(byte & 0x7f) << shift;
    i++;
    if ((byte & 0x80) == 0) { *out = result; return i; }
    shift += 7;
  }
  return 0;                            /* truncated */
}

/* Skip a field of the given wire type starting at b[0]. Returns bytes
 * consumed for the value, or 0 on error. */
static size_t skip_value(const unsigned char *b, size_t len, int wire) {
  uint64_t v;
  size_t n;
  switch (wire) {
    case 0: /* varint */
      return read_varint(b, len, &v);
    case 1: /* 64-bit */
      return len >= 8 ? 8 : 0;
    case 2: /* length-delimited */
      n = read_varint(b, len, &v);
      if (!n || v > (uint64_t)(len - n)) return 0;
      return n + (size_t)v;
    case 5: /* 32-bit */
      return len >= 4 ? 4 : 0;
    default:
      return 0;                        /* unknown/invalid wire type */
  }
}

static void store_port(struct portstats_snapshot *snap, int port,
                       uint64_t rxo, uint64_t rxp, uint64_t txo, uint64_t txp) {
  if (port <= 0 || port > PORTSTATS_MAX_PORTS) return;  /* invalid port ignored */
  struct port_counters *p = &snap->ports[port - 1];
  if (!p->present) snap->count++;      /* count distinct ports once */
  p->port = port;
  p->present = 1;
  p->rx_octets = rxo;
  p->rx_packets = rxp;
  p->tx_octets = txo;
  p->tx_packets = txp;
}

/* Parse one per-port submessage. Returns 0 on success, -1 on malformed. */
static int parse_port_submsg(const unsigned char *b, size_t len,
                             struct portstats_snapshot *snap) {
  int port = 0;
  uint64_t rxo = 0, rxp = 0, txo = 0, txp = 0;
  size_t i = 0;
  while (i < len) {
    uint64_t key;
    size_t n = read_varint(b + i, len - i, &key);
    if (!n) return -1;
    i += n;
    int field = (int)(key >> 3);
    int wire = (int)(key & 7);
    if (wire == 0) {
      uint64_t v;
      n = read_varint(b + i, len - i, &v);
      if (!n) return -1;
      i += n;
      switch (field) {
        case 1: port = (v > (uint64_t)PORTSTATS_MAX_PORTS) ? 0 : (int)v; break;
        case 3: rxo = v; break;
        case 4: rxp = v; break;
        case 13: txo = v; break;
        case 14: txp = v; break;
        default: break;                /* breakdown/unknown varints ignored */
      }
    } else {
      n = skip_value(b + i, len - i, wire);
      if (!n) return -1;
      i += n;
    }
  }
  store_port(snap, port, rxo, rxp, txo, txp);
  return 0;
}

int portstats_decode(const unsigned char *buf, size_t len,
                     struct portstats_snapshot *snap) {
  memset(snap, 0, sizeof(*snap));
  if (!buf || len == 0) return -1;

  size_t i = 0;
  while (i < len) {
    uint64_t key;
    size_t n = read_varint(buf + i, len - i, &key);
    if (!n) { memset(snap, 0, sizeof(*snap)); return -1; }
    i += n;
    int field = (int)(key >> 3);
    int wire = (int)(key & 7);
    if (field == 2 && wire == 0) {
      uint64_t v;
      n = read_varint(buf + i, len - i, &v);
      if (!n) { memset(snap, 0, sizeof(*snap)); return -1; }
      i += n;
      snap->timestamp = (long)v;
    } else if (field == 3 && wire == 2) {
      uint64_t plen;
      n = read_varint(buf + i, len - i, &plen);
      if (!n || plen > (uint64_t)(len - i - n)) { memset(snap, 0, sizeof(*snap)); return -1; }
      i += n;
      if (parse_port_submsg(buf + i, (size_t)plen, snap) != 0) {
        memset(snap, 0, sizeof(*snap));
        return -1;
      }
      i += (size_t)plen;
    } else {
      n = skip_value(buf + i, len - i, wire);
      if (!n) { memset(snap, 0, sizeof(*snap)); return -1; }
      i += n;
    }
  }
  snap->valid = 1;
  return 0;
}

#define PORTSTATS_MAX_BYTES (256 * 1024)

int portstats_read(struct portstats_snapshot *snap) {
  memset(snap, 0, sizeof(*snap));
  const char *path = getenv("CONFIGD_PORT_PROTOBUF");
  if (!path || !*path) path = "/click/switch_port_table/switch_port_protobuf";

  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;

  unsigned char *buf = malloc(PORTSTATS_MAX_BYTES);
  if (!buf) { close(fd); return -1; }

  size_t total = 0;
  for (;;) {
    if (total >= PORTSTATS_MAX_BYTES) { free(buf); close(fd); return -1; }
    ssize_t r = read(fd, buf + total, PORTSTATS_MAX_BYTES - total);
    if (r < 0) {
      if (errno == EINTR) continue;
      free(buf); close(fd); return -1;
    }
    if (r == 0) break;
    total += (size_t)r;
  }
  close(fd);

  int rc = portstats_decode(buf, total, snap);
  free(buf);
  if (rc == 0) snap->generated_unix = (long)time(NULL);
  return rc;
}

static const char *link_word(enum port_link_state s) {
  return s == PORT_LINK_UP ? "up" : "down";
}

int portstats_write_file(const struct portstats_snapshot *snap) {
  const char *path = getenv("CONFIGD_PORTSTATS_FILE");
  if (!path || !*path) path = "/run/postmerkos/portstats.v1";

  char tmp[512];
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;

  FILE *f = fopen(tmp, "w");
  if (!f) return -1;

  fprintf(f, "# postmerkos-portstats v1\n");
  fprintf(f, "# generated_unix=%ld\n", snap->generated_unix);
  fprintf(f, "# click_timestamp=%ld\n", snap->timestamp);
  fprintf(f, "# valid=%d\n", snap->valid);
  fprintf(f, "# ttl_seconds=%ld\n", snap->ttl_seconds);
  fprintf(f, "# discontinuity_ticks=%ld\n", snap->discontinuity_ticks);
  fprintf(f, "# columns=ifindex name admin oper speed_mbps rx_octets rx_packets "
             "rx_errors rx_discards tx_octets tx_packets tx_errors tx_discards "
             "rx_multicast rx_broadcast tx_multicast tx_broadcast\n");

  for (int idx = 0; idx < PORTSTATS_MAX_PORTS; idx++) {
    const struct port_counters *p = &snap->ports[idx];
    if (!p->present) continue;
    fprintf(f,
            "%d port%d %s %s %d "
            "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
            p->port, p->port, link_word(p->admin), link_word(p->oper),
            p->speed_mbps,
            (unsigned long long)p->rx_octets, (unsigned long long)p->rx_packets,
            (unsigned long long)p->rx_errors, (unsigned long long)p->rx_discards,
            (unsigned long long)p->tx_octets, (unsigned long long)p->tx_packets,
            (unsigned long long)p->tx_errors, (unsigned long long)p->tx_discards,
            (unsigned long long)p->rx_multicast, (unsigned long long)p->rx_broadcast,
            (unsigned long long)p->tx_multicast, (unsigned long long)p->tx_broadcast);
  }

  if (fflush(f) != 0) { fclose(f); unlink(tmp); return -1; }
  int fd = fileno(f);
  if (fd >= 0) fsync(fd);  /* best-effort; no-op on tmpfs */
  if (fclose(f) != 0) { unlink(tmp); return -1; }

  if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
  return 0;
}
