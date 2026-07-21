#include "cert.h"

#include <stdio.h>
#include <string.h>

/* Standalone boot helper: generate a self-signed TLS cert+key if the switch has
   none yet. Invoked from the S13postmerkos-tls init script. Kept tiny (links only
   cert.c + mbedTLS) so it adds negligible weight to the squashfs. */
int main(int argc, char **argv) {
  if (argc < 3 || argc > 4) {
    fprintf(stderr, "usage: %s <cert-path> <key-path> [common-name]\n", argv[0]);
    return 2;
  }
  const char *cn = argc == 4 ? argv[3] : "postmerkos";
  if (cert_generate_self_signed(argv[1], argv[2], cn) != 0) {
    fprintf(stderr, "gencert: failed to generate self-signed certificate\n");
    return 1;
  }
  return 0;
}
