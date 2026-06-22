#include "ssh_keys.h"
#include "config_file.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void set_err(char *error, size_t n, const char *msg) {
  if (error && n) snprintf(error, n, "%s", msg ? msg : "error");
}

static const char *ALLOWED_TYPES[] = {
  "ssh-ed25519", "ssh-rsa",
  "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
  "sk-ssh-ed25519@openssh.com", "sk-ecdsa-sha2-nistp256@openssh.com", NULL
};

static bool token(const char **pp, char *out, size_t n) {
  const char *p = *pp;
  while (*p == ' ' || *p == '\t') p++;
  const char *start = p;
  while (*p && *p != ' ' && *p != '\t') p++;
  size_t len = (size_t)(p - start);
  if (len == 0 || len >= n) { *pp = p; return false; }
  memcpy(out, start, len);
  out[len] = '\0';
  *pp = p;
  return true;
}

static bool type_allowed(const char *type) {
  for (size_t i = 0; ALLOWED_TYPES[i]; i++)
    if (!strcmp(type, ALLOWED_TYPES[i])) return true;
  return false;
}

static bool is_base64(const char *s) {
  if (!*s) return false;
  for (const char *p = s; *p; p++)
    if (!(isalnum((unsigned char)*p) || *p == '+' || *p == '/' || *p == '='))
      return false;
  return true;
}

int ssh_key_validate(const char *line, char *error, size_t n) {
  if (!line || !*line) { set_err(error, n, "key is empty"); return -EINVAL; }
  if (strpbrk(line, "\r\n")) {
    set_err(error, n, "key must be a single line"); return -EINVAL;
  }
  if (strlen(line) > 16384) { set_err(error, n, "key is too long"); return -EINVAL; }

  const char *p = line;
  char type[80], blob[16384];
  if (!token(&p, type, sizeof(type))) {
    set_err(error, n, "key type is missing"); return -EINVAL;
  }
  if (!type_allowed(type)) {
    set_err(error, n, "unsupported or option-prefixed key type"); return -EINVAL;
  }
  if (!token(&p, blob, sizeof(blob)) || strlen(blob) < 8 || !is_base64(blob)) {
    set_err(error, n, "key data is missing or not valid base64");
    return -EINVAL;
  }
  for (const char *c = p; *c; c++)
    if (iscntrl((unsigned char)*c)) {
      set_err(error, n, "comment contains control characters"); return -EINVAL;
    }
  return 0;
}

int ssh_key_identity(const char *line, char *out, size_t n) {
  if (!line) return -EINVAL;
  const char *p = line;
  char type[80], blob[16384];
  if (!token(&p, type, sizeof(type)) || !token(&p, blob, sizeof(blob)))
    return -EINVAL;
  if ((size_t)snprintf(out, n, "%s %s", type, blob) >= n) return -EINVAL;
  return 0;
}

/* Implemented in a later task; stubs keep the unit under test compilable. */
struct json_object *ssh_keys_list(void) { return json_object_new_array(); }
int ssh_keys_add(const char *label, const char *key, char *error, size_t n) {
  (void)label; (void)key; (void)error; (void)n; return -ENOSYS;
}
int ssh_keys_remove(const char *key, char *error, size_t n) {
  (void)key; (void)error; (void)n; return -ENOSYS;
}
int ssh_keys_render(char *error, size_t n) { (void)error; (void)n; return 0; }
