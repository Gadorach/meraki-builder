#include "portstats.h"

#include <string.h>

/* Read a base-128 varint. Returns bytes consumed, or 0 on error
 * (truncated, or more than 10 continuation bytes). */
static size_t read_varint(const unsigned char *b, size_t len, uint64_t *out) {
  uint64_t result = 0;
  size_t i = 0;
  int shift = 0;
  while (i < len) {
    unsigned char byte = b[i];
    if (i >= 10) return 0;            /* varint too long */
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
        case 1: port = (int)v; break;
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

/* portstats_read and portstats_write_file are implemented in later tasks. */
