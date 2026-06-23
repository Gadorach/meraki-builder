/*
 * Off-device unit test for the portstats.v1 parser embedded in the
 * mini_snmpd IF-MIB patch (0001-ifmib-from-portstats-file.patch).
 *
 * This mirrors the ps_read() / struct ps_port defined in that patch
 * (linux.c) so the parser logic can be validated without building the
 * full daemon.  Keep it in sync with the patch if the parser changes.
 *
 * Build & run:
 *   cc -std=gnu11 -Wall -Wextra -Werror -o ps_read_test ps_read_test.c && ./ps_read_test
 *
 * Expected output: "ps parser ok"
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_NR_INTERFACES 64

struct ps_port {
	int          ifindex;
	char         name[16];
	int          admin_up;
	int          oper_up;
	unsigned int speed_mbps;
	long long    rx_octets, rx_packets, tx_octets, tx_packets;
};

/* Verbatim copy of the parser from the patch (linux.c). */
static int ps_read(const char *path, struct ps_port *out, int max)
{
	FILE *f;
	char line[1024];
	long generated = 0, ttl = 0;
	int valid = 0, n = 0;

	f = fopen(path, "r");
	if (!f)
		return -1;

	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#') {
			sscanf(line, "# generated_unix=%ld", &generated);
			sscanf(line, "# ttl_seconds=%ld", &ttl);
			sscanf(line, "# valid=%d", &valid);
			continue;
		}
		if (n >= max)
			break;

		{
			struct ps_port p;
			char admin[8], oper[8];
			int got;

			memset(&p, 0, sizeof(p));
			got = sscanf(line,
				     "%d %15s %7s %7s %u %lld %lld %*u %*u %lld %lld",
				     &p.ifindex, p.name, admin, oper, &p.speed_mbps,
				     &p.rx_octets, &p.rx_packets,
				     &p.tx_octets, &p.tx_packets);
			if (got < 9)
				continue;	/* malformed row: skip */

			p.admin_up = strcmp(admin, "up") == 0;
			p.oper_up  = strcmp(oper, "up") == 0;
			out[n++] = p;
		}
	}
	fclose(f);

	if (!valid)
		return -1;
	if (ttl > 0 && (long)time(NULL) > generated + ttl)
		return -1;	/* stale */
	if (n == 0)
		return -1;	/* no usable rows */

	return n;
}

static void write_file(const char *path, const char *contents)
{
	FILE *f = fopen(path, "w");
	if (!f) {
		perror("fopen");
		exit(2);
	}
	fputs(contents, f);
	fclose(f);
}

int main(void)
{
	struct ps_port p[MAX_NR_INTERFACES];
	char buf[2048];
	int n;

	/* 1) Happy path: two present ports (1 and 3), values parsed exactly. */
	snprintf(buf, sizeof(buf),
		 "# postmerkos-portstats v1\n"
		 "# generated_unix=%ld\n"
		 "# ttl_seconds=999999\n"
		 "# valid=1\n"
		 "# columns=ifindex name admin oper speed_mbps rx_octets rx_packets rx_errors rx_discards tx_octets tx_packets tx_errors tx_discards rx_multicast rx_broadcast tx_multicast tx_broadcast\n"
		 "1 port1 up up 1000 3613875132 28117844 0 0 1020153062 13943591 0 0 0 0 0 0\n"
		 "3 port3 down down 10000 9999999999 5 0 0 8888888888 7 0 0 0 0 0 0\n",
		 (long)time(NULL));
	write_file("ps.v1", buf);

	n = ps_read("ps.v1", p, MAX_NR_INTERFACES);
	if (n != 2) {
		printf("FAIL: expected 2 ports, got %d\n", n);
		return 1;
	}
	if (p[0].ifindex != 1 || strcmp(p[0].name, "port1") ||
	    p[0].admin_up != 1 || p[0].oper_up != 1 || p[0].speed_mbps != 1000 ||
	    p[0].rx_octets != 3613875132LL || p[0].rx_packets != 28117844LL ||
	    p[0].tx_octets != 1020153062LL || p[0].tx_packets != 13943591LL) {
		printf("FAIL: port1 fields mismatch\n");
		return 1;
	}
	if (p[1].ifindex != 3 || strcmp(p[1].name, "port3") ||
	    p[1].admin_up != 0 || p[1].oper_up != 0 || p[1].speed_mbps != 10000 ||
	    p[1].rx_octets != 9999999999LL || p[1].tx_octets != 8888888888LL) {
		printf("FAIL: port3 fields mismatch\n");
		return 1;
	}

	/* 2) Stale: now > generated + ttl  ->  -1 */
	write_file("ps_stale.v1",
		   "# generated_unix=1\n"
		   "# ttl_seconds=15\n"
		   "# valid=1\n"
		   "1 port1 up up 1000 1 1 0 0 1 1 0 0 0 0 0 0\n");
	if (ps_read("ps_stale.v1", p, MAX_NR_INTERFACES) != -1) {
		printf("FAIL: stale file not rejected\n");
		return 1;
	}

	/* 3) Missing file  ->  -1 */
	if (ps_read("does-not-exist.v1", p, MAX_NR_INTERFACES) != -1) {
		printf("FAIL: missing file not rejected\n");
		return 1;
	}

	/* 4) valid=0  ->  -1 */
	write_file("ps_invalid.v1",
		   "# generated_unix=9999999999\n"
		   "# ttl_seconds=15\n"
		   "# valid=0\n"
		   "1 port1 up up 1000 1 1 0 0 1 1 0 0 0 0 0 0\n");
	if (ps_read("ps_invalid.v1", p, MAX_NR_INTERFACES) != -1) {
		printf("FAIL: valid=0 file not rejected\n");
		return 1;
	}

	/* 5) Header present, no data rows  ->  -1 (no usable rows) */
	write_file("ps_empty.v1",
		   "# generated_unix=9999999999\n"
		   "# ttl_seconds=15\n"
		   "# valid=1\n");
	if (ps_read("ps_empty.v1", p, MAX_NR_INTERFACES) != -1) {
		printf("FAIL: empty file not rejected\n");
		return 1;
	}

	/* 6) Malformed rows skipped, valid rows kept. */
	snprintf(buf, sizeof(buf),
		 "# generated_unix=%ld\n# ttl_seconds=999999\n# valid=1\n"
		 "garbage line that is not a row\n"
		 "2 port2 up up\n"           /* too few columns -> skipped */
		 "7 port7 up up 1000 100 2 0 0 200 3 0 0 0 0 0 0\n",
		 (long)time(NULL));
	write_file("ps_mixed.v1", buf);
	n = ps_read("ps_mixed.v1", p, MAX_NR_INTERFACES);
	if (n != 1 || p[0].ifindex != 7 || p[0].rx_octets != 100LL ||
	    p[0].tx_octets != 200LL) {
		printf("FAIL: malformed handling, n=%d\n", n);
		return 1;
	}

	puts("ps parser ok");
	return 0;
}
