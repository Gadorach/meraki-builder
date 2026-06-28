#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SESSION_MAX 32

struct session_entry {
  char token[65];
  char username[65];
  long expires_at;
};

static struct session_entry table[SESSION_MAX];

static void purge_expired(long now) {
  for (size_t i = 0; i < SESSION_MAX; i++)
    if (table[i].token[0] && table[i].expires_at <= now) table[i].token[0] = '\0';
}

/* Length-aware constant-time string compare. */
static int ct_equal(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  unsigned char diff = (unsigned char)((la ^ lb) != 0);
  size_t n = la < lb ? la : lb;
  for (size_t i = 0; i < n; i++) diff |= (unsigned char)(a[i] ^ b[i]);
  return diff == 0;
}

static int generate_token(char out[65]) {
  const char *path = getenv("CONFIGD_URANDOM");
  if (!path || !*path) path = "/dev/urandom";
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  unsigned char raw[32];
  size_t got = 0;
  while (got < sizeof(raw)) {
    ssize_t r = read(fd, raw + got, sizeof(raw) - got);
    if (r <= 0) { if (r < 0 && errno == EINTR) continue; close(fd); return -1; }
    got += (size_t)r;
  }
  close(fd);
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(raw); i++) {
    out[i * 2] = hex[raw[i] >> 4];
    out[i * 2 + 1] = hex[raw[i] & 0x0f];
  }
  out[64] = '\0';
  return 0;
}

const char *session_create(const char *username, long ttl_seconds, long now) {
  if (!username || !*username) return NULL;
  purge_expired(now);
  int slot = -1;
  long oldest = 0;
  for (size_t i = 0; i < SESSION_MAX; i++) {
    if (!table[i].token[0]) { slot = (int)i; break; }
    if (slot < 0 || table[i].expires_at < oldest) { oldest = table[i].expires_at; slot = (int)i; }
  }
  if (generate_token(table[slot].token) != 0) { table[slot].token[0] = '\0'; return NULL; }
  snprintf(table[slot].username, sizeof(table[slot].username), "%s", username);
  table[slot].expires_at = now + ttl_seconds;
  return table[slot].token;
}

int session_lookup(const char *token, long now, char *out_username, size_t n) {
  if (!token || strlen(token) != 64) return -1;
  purge_expired(now);
  for (size_t i = 0; i < SESSION_MAX; i++) {
    if (table[i].token[0] && ct_equal(table[i].token, token)) {
      if (out_username && n) snprintf(out_username, n, "%s", table[i].username);
      return 0;
    }
  }
  return -1;
}

void session_revoke_token(const char *token) {
  if (!token || !*token) return;
  for (size_t i = 0; i < SESSION_MAX; i++)
    if (table[i].token[0] && ct_equal(table[i].token, token)) table[i].token[0] = '\0';
}

void session_revoke_user(const char *username) {
  if (!username || !*username) return;
  for (size_t i = 0; i < SESSION_MAX; i++)
    if (table[i].token[0] && !strcmp(table[i].username, username)) table[i].token[0] = '\0';
}
