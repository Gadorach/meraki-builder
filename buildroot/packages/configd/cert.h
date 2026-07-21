#ifndef CONFIGD_CERT_H
#define CONFIGD_CERT_H

#include <stddef.h>

/* TLS certificate primitives (mbedTLS-backed) shared by configd and the
   standalone `gencert` boot helper. Keep this dependency-light: no json-c here so
   `gencert` links only mbedTLS. */

struct cert_info {
  char subject[256];      /* RFC4514 DN, e.g. "CN=Q2XX-...." */
  char issuer[256];
  char fingerprint[96];   /* uppercase hex SHA-256 of the DER, colon-separated */
  char not_before[24];    /* "YYYY-MM-DD HH:MM:SS" (UTC) */
  char not_after[24];
  int  self_signed;       /* 1 if subject == issuer */
};

/* Generate a self-signed cert+key (EC P-256, SHA-256, ~20y fixed window) and write them PEM to
   the given paths (key mode 0600, cert 0644). Returns 0 on success, negative on
   error. `cn` is the certificate Common Name (e.g. the device serial/hostname). */
int cert_generate_self_signed(const char *cert_path, const char *key_path,
                              const char *cn);

/* Fill `out` from the PEM certificate at `cert_path`. Returns 0 on success. */
int cert_read_info(const char *cert_path, struct cert_info *out);

/* Validate that `cert_pem`/`key_pem` are well-formed PEM and that the private key
   matches the certificate's public key. Returns 0 on success; on failure returns
   negative and writes a human-readable reason into `err` (if non-NULL). */
int cert_validate_pair_pem(const char *cert_pem, const char *key_pem,
                           char *err, size_t err_size);

/* Like cert_validate_pair_pem but for the on-disk pair at cert_path/key_path
   (what pmweb actually consumes): returns 0 iff the cert parses, the key parses,
   and they match; otherwise negative with a reason in `err`. Used so cert_get can
   report whether HTTPS will actually come up, not merely that a cert file exists. */
int cert_check_pair_files(const char *cert_path, const char *key_path,
                          char *err, size_t err_size);

/* Atomically install a validated cert+key PEM pair to the given paths (creating
   the parent directory). key is written 0600, cert 0644. Returns 0 on success. */
int cert_install_pair_pem(const char *cert_path, const char *key_path,
                          const char *cert_pem, const char *key_pem);

#endif
