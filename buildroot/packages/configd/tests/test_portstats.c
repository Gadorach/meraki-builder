#include "../portstats.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- minimal protobuf encoder used only to build test fixtures --- */
static size_t enc_varint(unsigned char *out, uint64_t v) {
  size_t i = 0;
  do { unsigned char b = v & 0x7f; v >>= 7; if (v) b |= 0x80; out[i++] = b; } while (v);
  return i;
}
static size_t enc_key(unsigned char *out, int field, int wire) {
  return enc_varint(out, ((uint64_t)field << 3) | (uint64_t)wire);
}
/* append varint field (wire type 0) */
static size_t put_varint_field(unsigned char *out, int field, uint64_t v) {
  size_t n = enc_key(out, field, 0);
  n += enc_varint(out + n, v);
  return n;
}
/* append a length-delimited field (wire type 2) wrapping payload */
static size_t put_ld_field(unsigned char *out, int field,
                           const unsigned char *payload, size_t plen) {
  size_t n = enc_key(out, field, 2);
  n += enc_varint(out + n, plen);
  memcpy(out + n, payload, plen);
  return n + plen;
}
/* build one per-port submessage */
static size_t build_port(unsigned char *out, int port,
                         uint64_t rxo, uint64_t rxp, uint64_t txo, uint64_t txp) {
  size_t n = 0;
  n += put_varint_field(out + n, 1, (uint64_t)port);
  n += put_varint_field(out + n, 3, rxo);
  n += put_varint_field(out + n, 4, rxp);
  n += put_varint_field(out + n, 13, txo);
  n += put_varint_field(out + n, 14, txp);
  return n;
}

int main(void) {
  unsigned char sub[128];
  unsigned char msg[1024];
  size_t m = 0;

  /* top-level field 2 = timestamp */
  m += put_varint_field(msg + m, 2, 1453835814ULL);

  size_t s;
  s = build_port(sub, 1, 3613875132ULL, 28117844ULL, 1020153062ULL, 13943591ULL);
  m += put_ld_field(msg + m, 3, sub, s);
  s = build_port(sub, 3, 55873ULL, 293ULL, 9285133ULL, 101505ULL);
  m += put_ld_field(msg + m, 3, sub, s);
  s = build_port(sub, 5, 472732ULL, 1608ULL, 10120845ULL, 106084ULL);
  m += put_ld_field(msg + m, 3, sub, s);

  struct portstats_snapshot snap;
  assert(portstats_decode(msg, m, &snap) == 0);
  assert(snap.valid == 1);
  assert(snap.timestamp == 1453835814L);
  assert(snap.count == 3);

  /* ports are stored by 1-based index: ports[port-1] */
  assert(snap.ports[0].present && snap.ports[0].port == 1);
  assert(snap.ports[0].rx_octets == 3613875132ULL);
  assert(snap.ports[0].rx_packets == 28117844ULL);
  assert(snap.ports[0].tx_octets == 1020153062ULL);
  assert(snap.ports[0].tx_packets == 13943591ULL);
  assert(snap.ports[2].present && snap.ports[2].port == 3);
  assert(snap.ports[2].rx_octets == 55873ULL);
  assert(snap.ports[4].present && snap.ports[4].port == 5);
  assert(snap.ports[4].tx_packets == 106084ULL);
  /* gaps are absent */
  assert(snap.ports[1].present == 0);

  /* empty buffer -> invalid but no crash */
  struct portstats_snapshot empty;
  assert(portstats_decode((const unsigned char *)"", 0, &empty) != 0);
  assert(empty.valid == 0);

  /* garbage -> invalid */
  struct portstats_snapshot garbage;
  unsigned char g[] = {0xff, 0xff, 0xff, 0xff};
  assert(portstats_decode(g, sizeof(g), &garbage) != 0);

  /* truncated length-delimited: claim length beyond buffer */
  struct portstats_snapshot trunc;
  unsigned char t[8]; size_t tn = 0;
  tn += enc_key(t + tn, 3, 2);
  tn += enc_varint(t + tn, 99);  /* says 99 bytes follow, but none do */
  assert(portstats_decode(t, tn, &trunc) != 0);

  /* huge varint (>10 bytes continuation) -> invalid, no overflow */
  struct portstats_snapshot huge;
  unsigned char h[16]; size_t hn = 0;
  hn += enc_key(h + hn, 2, 0);
  for (int i = 0; i < 11; i++) h[hn++] = 0x80;  /* never terminates */
  assert(portstats_decode(h, hn, &huge) != 0);

  /* unknown valid field (field 7, wire 2) is skipped safely */
  struct portstats_snapshot unk;
  unsigned char u[256]; size_t un = 0;
  un += put_varint_field(u + un, 2, 42ULL);
  unsigned char pad[3] = {1, 2, 3};
  un += put_ld_field(u + un, 7, pad, sizeof(pad));
  s = build_port(sub, 2, 10, 1, 20, 2);
  un += put_ld_field(u + un, 3, sub, s);
  assert(portstats_decode(u, un, &unk) == 0);
  assert(unk.ports[1].present && unk.ports[1].rx_octets == 10ULL);

  /* port 0 ignored; port > MAX ignored; snapshot still valid */
  struct portstats_snapshot oob;
  unsigned char o[256]; size_t on = 0;
  on += put_varint_field(o + on, 2, 1ULL);
  s = build_port(sub, 0, 5, 5, 5, 5);   on += put_ld_field(o + on, 3, sub, s);
  s = build_port(sub, 999, 5, 5, 5, 5); on += put_ld_field(o + on, 3, sub, s);
  s = build_port(sub, 4, 7, 7, 7, 7);   on += put_ld_field(o + on, 3, sub, s);
  assert(portstats_decode(o, on, &oob) == 0);
  assert(oob.count == 1);
  assert(oob.ports[3].present && oob.ports[3].rx_octets == 7ULL);

  /* duplicate port: last wins */
  struct portstats_snapshot dup;
  unsigned char d[256]; size_t dn = 0;
  dn += put_varint_field(d + dn, 2, 1ULL);
  s = build_port(sub, 6, 100, 1, 1, 1); dn += put_ld_field(d + dn, 3, sub, s);
  s = build_port(sub, 6, 200, 2, 2, 2); dn += put_ld_field(d + dn, 3, sub, s);
  assert(portstats_decode(d, dn, &dup) == 0);
  assert(dup.ports[5].rx_octets == 200ULL);
  assert(dup.count == 1);

  /* zero-length port submessage: valid, no bogus port stored */
  struct portstats_snapshot zlen;
  unsigned char z[128]; size_t zn = 0;
  zn += put_varint_field(z + zn, 2, 42ULL);
  zn += put_ld_field(z + zn, 3, sub, 0);  /* plen=0 */
  assert(portstats_decode(z, zn, &zlen) == 0);
  assert(zlen.valid == 1);
  assert(zlen.count == 0);

  /* port submessage with unknown length-delimited field (wire-type 2) is skipped */
  struct portstats_snapshot skip;
  unsigned char sk[256]; size_t skn = 0;
  skn += put_varint_field(sk + skn, 2, 99ULL);
  /* build a port submessage that includes field 9 (wire-type 2, unknown) */
  size_t ps = 0;
  ps += put_varint_field(sub + ps, 1, 7ULL);      /* port 7 */
  ps += put_varint_field(sub + ps, 3, 12345ULL);  /* rx_octets */
  unsigned char pad_ld[5] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee};
  ps += put_ld_field(sub + ps, 9, pad_ld, sizeof(pad_ld)); /* unknown LD field */
  ps += put_varint_field(sub + ps, 4, 999ULL);    /* rx_packets */
  skn += put_ld_field(sk + skn, 3, sub, ps);
  assert(portstats_decode(sk, skn, &skip) == 0);
  assert(skip.ports[6].present && skip.ports[6].port == 7);
  assert(skip.ports[6].rx_octets == 12345ULL);
  assert(skip.ports[6].rx_packets == 999ULL);

  /* portstats_read: write a fixture file, point env at it, decode it */
  {
    char path[256];
    snprintf(path, sizeof(path), "%s/portstats-fixture.bin",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(msg, 1, m, f) == m);
    fclose(f);
    setenv("CONFIGD_PORT_PROTOBUF", path, 1);

    struct portstats_snapshot rd;
    assert(portstats_read(&rd) == 0);
    assert(rd.valid == 1);
    assert(rd.ports[0].rx_octets == 3613875132ULL);

    /* missing file -> invalid, no crash */
    setenv("CONFIGD_PORT_PROTOBUF", "/nonexistent/path/xyz", 1);
    struct portstats_snapshot miss;
    assert(portstats_read(&miss) != 0);
    assert(miss.valid == 0);
    unsetenv("CONFIGD_PORT_PROTOBUF");
  }

  /* portstats_write_file: render and re-read the v1 file */
  {
    /* enrich the earlier snapshot with oper/admin/speed */
    snap.ports[0].oper = PORT_LINK_UP;
    snap.ports[0].admin = PORT_LINK_UP;
    snap.ports[0].speed_mbps = 1000;
    snap.generated_unix = 1782230000L;
    snap.timestamp = 123456L;
    snap.ttl_seconds = 15L;
    snap.discontinuity_ticks = 123400L;

    char path[256];
    snprintf(path, sizeof(path), "%s/portstats.v1",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    setenv("CONFIGD_PORTSTATS_FILE", path, 1);
    assert(portstats_write_file(&snap) == 0);

    FILE *f = fopen(path, "r");
    assert(f);
    char content[4096];
    size_t got = fread(content, 1, sizeof(content) - 1, f);
    fclose(f);
    content[got] = '\0';

    assert(strstr(content, "# postmerkos-portstats v1"));
    assert(strstr(content, "generated_unix=1782230000"));
    assert(strstr(content, "ttl_seconds=15"));
    assert(strstr(content, "# columns=ifindex name admin oper speed_mbps"));
    /* port 1 row: ifindex name admin oper speed rx_octets ... */
    assert(strstr(content, "1 port1 up up 1000 3613875132 28117844"));
    unsetenv("CONFIGD_PORTSTATS_FILE");
  }

  puts("portstats decode tests passed");
  return 0;
}
