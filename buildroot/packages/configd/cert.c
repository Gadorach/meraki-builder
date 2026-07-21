#include "cert.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

/* Fixed, wide validity window: switches often boot with a wrong clock (pre-NTP),
   so a fixed self-signed window avoids "not yet valid"/"expired" surprises. */
#define CERT_NOT_BEFORE "20240101000000"
#define CERT_NOT_AFTER  "20440101000000"

static void set_err(char *err, size_t size, const char *message) {
  if (err && size) snprintf(err, size, "%s", message);
}

/* Defined below; used by cert_generate_self_signed to publish its output as a
   single validated, atomically-committed cert+key pair. */
static int commit_pair(const char *cert_path, const char *key_path,
                       const unsigned char *cert, size_t cert_len,
                       const unsigned char *key, size_t key_len);

static int write_file(const char *path, const unsigned char *data, size_t len,
                      mode_t mode) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) return -errno;
  int rc = 0;
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, data + off, len - off);
    if (n < 0) { if (errno == EINTR) continue; rc = -errno; break; }
    off += (size_t)n;
  }
  if (rc == 0 && fsync(fd) != 0) rc = -errno;
  if (close(fd) != 0 && rc == 0) rc = -errno;
  return rc;
}

int cert_generate_self_signed(const char *cert_path, const char *key_path,
                              const char *cn) {
  int ret = -1;
  mbedtls_pk_context key;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_entropy_context entropy;
  mbedtls_x509write_cert crt;
  mbedtls_mpi serial;
  const char *pers = "configd-gencert";
  char name[300];
  unsigned char keybuf[2048];
  unsigned char certbuf[4096];

  mbedtls_pk_init(&key);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_entropy_init(&entropy);
  mbedtls_x509write_crt_init(&crt);
  mbedtls_mpi_init(&serial);

  if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                            (const unsigned char *)pers, strlen(pers)) != 0)
    goto done;
  if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0)
    goto done;
  if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                          mbedtls_ctr_drbg_random, &ctr_drbg) != 0)
    goto done;
  if (mbedtls_pk_write_key_pem(&key, keybuf, sizeof(keybuf)) != 0) goto done;

  if (mbedtls_mpi_read_string(&serial, 10, "1") != 0) goto done;
  snprintf(name, sizeof(name), "CN=%s", (cn && *cn) ? cn : "postmerkos");

  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, &key);
  mbedtls_x509write_crt_set_issuer_key(&crt, &key);
  if (mbedtls_x509write_crt_set_subject_name(&crt, name) != 0) goto done;
  if (mbedtls_x509write_crt_set_issuer_name(&crt, name) != 0) goto done;
  if (mbedtls_x509write_crt_set_serial(&crt, &serial) != 0) goto done;
  if (mbedtls_x509write_crt_set_validity(&crt, CERT_NOT_BEFORE, CERT_NOT_AFTER) != 0)
    goto done;
  mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);

  memset(certbuf, 0, sizeof(certbuf));
  if (mbedtls_x509write_crt_pem(&crt, certbuf, sizeof(certbuf),
                                mbedtls_ctr_drbg_random, &ctr_drbg) != 0)
    goto done;

  /* Commit both files as an atomic, validated pair (see commit_pair). */
  if (commit_pair(cert_path, key_path, certbuf, strlen((char *)certbuf),
                  keybuf, strlen((char *)keybuf)) != 0)
    goto done;
  ret = 0;

done:
  mbedtls_platform_zeroize(keybuf, sizeof(keybuf));
  mbedtls_mpi_free(&serial);
  mbedtls_x509write_crt_free(&crt);
  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  return ret;
}

static void fmt_time(char *out, size_t size, const mbedtls_x509_time *t) {
  snprintf(out, size, "%04d-%02d-%02d %02d:%02d:%02d", t->year, t->mon, t->day,
           t->hour, t->min, t->sec);
}

int cert_read_info(const char *cert_path, struct cert_info *out) {
  if (!out) return -EINVAL;
  memset(out, 0, sizeof(*out));
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  if (mbedtls_x509_crt_parse_file(&crt, cert_path) != 0) {
    mbedtls_x509_crt_free(&crt);
    return -1;
  }
  mbedtls_x509_dn_gets(out->subject, sizeof(out->subject), &crt.subject);
  mbedtls_x509_dn_gets(out->issuer, sizeof(out->issuer), &crt.issuer);
  fmt_time(out->not_before, sizeof(out->not_before), &crt.valid_from);
  fmt_time(out->not_after, sizeof(out->not_after), &crt.valid_to);
  out->self_signed = strcmp(out->subject, out->issuer) == 0 ? 1 : 0;

  unsigned char hash[32];
  if (mbedtls_sha256_ret(crt.raw.p, crt.raw.len, hash, 0) == 0) {
    char *w = out->fingerprint;
    for (int i = 0; i < 32 && (size_t)(w - out->fingerprint) + 4 < sizeof(out->fingerprint); i++)
      w += snprintf(w, 4, i ? ":%02X" : "%02X", hash[i]);
  }
  mbedtls_x509_crt_free(&crt);
  return 0;
}

static void mkdir_parents(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", path);
  char *slash = strrchr(tmp, '/');
  if (!slash || slash == tmp) return;
  *slash = '\0';
  for (char *p = tmp + 1; *p; p++)
    if (*p == '/') { *p = '\0'; mkdir(tmp, 0700); *p = '/'; }
  mkdir(tmp, 0700);
  chmod(tmp, 0700);  // enforce 0700 even if the dir pre-existed world-readable
}

int cert_check_pair_files(const char *cert_path, const char *key_path, char *err,
                          size_t err_size) {
  mbedtls_x509_crt crt;
  mbedtls_pk_context pk;
  mbedtls_x509_crt_init(&crt);
  mbedtls_pk_init(&pk);
  int ret = -1;
  if (mbedtls_x509_crt_parse_file(&crt, cert_path) != 0) {
    set_err(err, err_size, "certificate is missing or not valid PEM");
  } else if (mbedtls_pk_parse_keyfile(&pk, key_path, NULL) != 0) {
    set_err(err, err_size, "private key is missing or unreadable");
  } else if (mbedtls_pk_check_pair(&crt.pk, &pk) != 0) {
    set_err(err, err_size, "private key does not match certificate");
  } else {
    ret = 0;
  }
  mbedtls_pk_free(&pk);
  mbedtls_x509_crt_free(&crt);
  return ret;
}

/* Install a cert+key pair as a unit: stage both to .tmp, validate the staged
   files parse and match, back up any existing pair to .bak, then rename both into
   place back-to-back (two adjacent rename() calls, no I/O between). If the second
   rename fails, roll both files back from .bak. This guarantees the on-disk pair
   is never left as new-key + old-cert (or a half-written file); the worst case is
   a sub-millisecond window where one file is briefly absent, which consumers
   (pmweb) treat as "no valid cert" and fall back to HTTP — not a TLS failure.
   An exclusive flock serializes concurrent cert_set/cert_delete (both go through
   here), so the shared .bak slot and PID-suffixed .tmp files never race. */
static int commit_pair(const char *cert_path, const char *key_path,
                       const unsigned char *cert, size_t cert_len,
                       const unsigned char *key, size_t key_len) {
  char cert_tmp[560], key_tmp[560], cert_bak[540], key_bak[540], lock_path[540];
  int rc, had_cert = 0, had_key = 0, lock_fd;
  snprintf(cert_tmp, sizeof(cert_tmp), "%s.tmp.%ld", cert_path, (long) getpid());
  snprintf(key_tmp, sizeof(key_tmp), "%s.tmp.%ld", key_path, (long) getpid());
  snprintf(cert_bak, sizeof(cert_bak), "%s.bak", cert_path);
  snprintf(key_bak, sizeof(key_bak), "%s.bak", key_path);
  snprintf(lock_path, sizeof(lock_path), "%s.lock", key_path);

  mkdir_parents(key_path);
  mkdir_parents(cert_path);

  /* Fail closed: without the lock we cannot guarantee serialization, so refuse
     rather than risk a concurrent writer racing on the shared .bak slot. */
  lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
  if (lock_fd < 0) return -errno;
  if (flock(lock_fd, LOCK_EX) != 0) { rc = -errno; close(lock_fd); return rc; }

  rc = write_file(key_tmp, key, key_len, 0600);
  if (rc == 0) rc = write_file(cert_tmp, cert, cert_len, 0644);
  if (rc != 0) { unlink(key_tmp); unlink(cert_tmp); goto out; }

  if (cert_check_pair_files(cert_tmp, key_tmp, NULL, 0) != 0) {
    unlink(key_tmp); unlink(cert_tmp); rc = -EINVAL; goto out;
  }

  /* Back up any existing pair (ENOENT on first install is fine). */
  had_cert = (rename(cert_path, cert_bak) == 0);
  had_key = (rename(key_path, key_bak) == 0);

  if (rename(cert_tmp, cert_path) != 0) {
    rc = -errno;
    if (had_cert) rename(cert_bak, cert_path);
    if (had_key) rename(key_bak, key_path);
    unlink(key_tmp);
    goto out;
  }
  if (rename(key_tmp, key_path) != 0) {
    rc = -errno;
    unlink(cert_path);                        /* undo the cert we just placed */
    if (had_cert) rename(cert_bak, cert_path);
    if (had_key) rename(key_bak, key_path);
    goto out;
  }
  if (had_cert) unlink(cert_bak);
  if (had_key) unlink(key_bak);
  rc = 0;

out:
  flock(lock_fd, LOCK_UN);
  close(lock_fd);
  return rc;
}

int cert_install_pair_pem(const char *cert_path, const char *key_path,
                          const char *cert_pem, const char *key_pem) {
  if (!cert_path || !key_path || !cert_pem || !key_pem) return -EINVAL;
  return commit_pair(cert_path, key_path,
                     (const unsigned char *)cert_pem, strlen(cert_pem),
                     (const unsigned char *)key_pem, strlen(key_pem));
}

int cert_validate_pair_pem(const char *cert_pem, const char *key_pem, char *err,
                           size_t err_size) {
  if (!cert_pem || !key_pem) { set_err(err, err_size, "missing cert or key"); return -EINVAL; }
  mbedtls_x509_crt crt;
  mbedtls_pk_context key;
  mbedtls_x509_crt_init(&crt);
  mbedtls_pk_init(&key);
  int ret = -1;

  /* PEM lengths passed to mbedTLS must include the terminating NUL. */
  if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)cert_pem,
                             strlen(cert_pem) + 1) != 0) {
    set_err(err, err_size, "certificate is not valid PEM");
    goto done;
  }
  if (mbedtls_pk_parse_key(&key, (const unsigned char *)key_pem,
                           strlen(key_pem) + 1, NULL, 0) != 0) {
    set_err(err, err_size, "private key is invalid or password-protected");
    goto done;
  }
  if (mbedtls_pk_check_pair(&crt.pk, &key) != 0) {
    set_err(err, err_size, "private key does not match certificate");
    goto done;
  }
  ret = 0;

done:
  mbedtls_pk_free(&key);
  mbedtls_x509_crt_free(&crt);
  return ret;
}
