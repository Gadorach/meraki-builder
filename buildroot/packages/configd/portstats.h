#ifndef POSTMERKOS_PORTSTATS_H
#define POSTMERKOS_PORTSTATS_H

#include <stddef.h>
#include <stdint.h>

#define PORTSTATS_MAX_PORTS 52

/* oper/admin link state, populated by the glue layer (not the decoder). */
enum port_link_state {
  PORT_LINK_UNKNOWN = 0,
  PORT_LINK_DOWN = 1,
  PORT_LINK_UP = 2
};

struct port_counters {
  int port;          /* protobuf field 1, 1-based; 0 = unused slot */
  int present;       /* 1 if this port appeared in the snapshot */

  uint64_t rx_octets;   /* field 3  */
  uint64_t rx_packets;  /* field 4  */
  uint64_t tx_octets;   /* field 13 */
  uint64_t tx_packets;  /* field 14 */

  /* Reserved for breakdown fields; remain 0 until pinned on-device. */
  uint64_t rx_errors;
  uint64_t tx_errors;
  uint64_t rx_discards;
  uint64_t tx_discards;
  uint64_t rx_multicast;
  uint64_t tx_multicast;
  uint64_t rx_broadcast;
  uint64_t tx_broadcast;

  /* Populated by the glue layer from existing status sources, not by decode. */
  enum port_link_state oper;   /* observed link state */
  enum port_link_state admin;  /* configured admin enable/disable */
  int speed_mbps;              /* link speed; 0 if down/unknown */
  int poe_present;             /* 1 if PoE power reading is valid */
  double poe_power_watts;      /* per-port PoE draw; valid iff poe_present */
};

struct portstats_snapshot {
  long timestamp;            /* top-level field 2 (Click clock) */
  long generated_unix;       /* wall-clock when the snapshot was taken */
  long ttl_seconds;          /* SNMP stats-file validity window */
  long discontinuity_ticks;  /* sysUpTime-style tick at last counter reset */

  int count;  /* number of present ports */
  int valid;  /* 1 = decode OK, 0 = read/parse failure */

  struct port_counters ports[PORTSTATS_MAX_PORTS];
};

/*
 * Decode an in-memory protobuf buffer (the host-testable seam).
 * Fills counters, port numbers, present flags, timestamp, count, valid.
 * Returns 0 on success, -1 on parse failure (snap->valid set to 0).
 */
int portstats_decode(const unsigned char *buf, size_t len,
                     struct portstats_snapshot *snap);

/*
 * Read $CONFIGD_PORT_PROTOBUF (default
 * "/click/switch_port_table/switch_port_protobuf") as raw bytes and decode.
 * Returns 0 on success, -1 on read/decode failure (snap->valid set to 0).
 */
int portstats_read(struct portstats_snapshot *snap);

/*
 * Render the SNMP feed file atomically to $CONFIGD_PORTSTATS_FILE
 * (default "/run/postmerkos/portstats.v1"). The snapshot must already be
 * fully populated (counters + oper/admin/speed/poe). Returns 0 / -1.
 */
int portstats_write_file(const struct portstats_snapshot *snap);

#endif
