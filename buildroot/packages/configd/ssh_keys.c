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

#define SSH_KEY_MAX_LEN 16384

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
  if (strlen(line) > SSH_KEY_MAX_LEN) { set_err(error, n, "key is too long"); return -EINVAL; }

  const char *p = line;
  char type[80], blob[SSH_KEY_MAX_LEN];
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
  char type[80], blob[SSH_KEY_MAX_LEN];
  if (!token(&p, type, sizeof(type)) || !token(&p, blob, sizeof(blob)))
    return -EINVAL;
  if ((size_t)snprintf(out, n, "%s %s", type, blob) >= n) return -EINVAL;
  return 0;
}

static struct json_object *keys_array(struct json_object *cfg, bool create) {
  struct json_object *ssh = NULL, *arr = NULL;
  if (!json_object_object_get_ex(cfg, "ssh", &ssh)) {
    if (!create) return NULL;
    ssh = json_object_new_object();
    json_object_object_add(cfg, "ssh", ssh);
  }
  if (!json_object_object_get_ex(ssh, "authorized_keys", &arr)) {
    if (!create) return NULL;
    arr = json_object_new_array();
    json_object_object_add(ssh, "authorized_keys", arr);
  }
  return arr;
}

struct json_object *ssh_keys_list(void) {
  struct json_object *out = json_object_new_array();
  struct json_object *cfg = load_config_file();
  if (!cfg) return out;
  struct json_object *arr = keys_array(cfg, false);
  for (size_t i = 0; arr && i < json_object_array_length(arr); i++) {
    struct json_object *e = json_object_array_get_idx(arr, i), *jk = NULL, *jl = NULL;
    if (!json_object_object_get_ex(e, "key", &jk)) continue;
    struct json_object *row = json_object_new_object();
    json_object_object_add(row, "label",
        json_object_new_string(json_object_object_get_ex(e, "label", &jl)
                               ? json_object_get_string(jl) : ""));
    json_object_object_add(row, "key",
        json_object_new_string(json_object_get_string(jk)));
    json_object_array_add(out, row);
  }
  json_object_put(cfg);
  return out;
}

static void fsync_parent_dir(const char *path) {
  char dir[640];
  if ((size_t)snprintf(dir, sizeof(dir), "%s", path) >= sizeof(dir)) return;
  char *slash = strrchr(dir, '/');
  if (!slash) return;
  if (slash == dir) slash[1] = '\0';
  else *slash = '\0';
  int fd = open(dir, O_RDONLY | O_DIRECTORY);
  if (fd < 0) return;
  (void)fsync(fd);
  close(fd);
}

static int write_atomic(const char *path, const char *text, mode_t mode) {
  char temp[640];
  if ((size_t)snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid())
      >= sizeof(temp)) return -ENAMETOOLONG;
  int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, mode);
  if (fd < 0) return -errno;
  int rc = 0;
  size_t remaining = strlen(text); const char *cursor = text;
  while (remaining) {
    ssize_t w = write(fd, cursor, remaining);
    if (w < 0) { if (errno == EINTR) continue; rc = -errno; break; }
    cursor += w; remaining -= (size_t)w;
  }
  if (rc == 0 && fsync(fd) != 0) rc = -errno;
  if (close(fd) != 0 && rc == 0) rc = -errno;
  if (rc == 0 && rename(temp, path) != 0) rc = -errno;
  if (rc == 0) fsync_parent_dir(path);
  else unlink(temp);
  return rc;
}

int ssh_keys_render(char *error, size_t n) {
  const char *dir = getenv("CONFIGD_SSH_DIR");
  if (!dir || !*dir) dir = "/root/.ssh";
  const char *file = getenv("CONFIGD_AUTHORIZED_KEYS");
  if (!file || !*file) file = "/root/.ssh/authorized_keys";

  if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
    set_err(error, n, strerror(errno)); return -errno;
  }
  (void)chmod(dir, 0700);

  struct json_object *cfg = load_config_file();
  size_t cap = 4096, len = 0;
  char *body = malloc(cap);
  if (!body) { if (cfg) json_object_put(cfg); set_err(error, n, "out of memory"); return -ENOMEM; }
  body[0] = '\0';
  struct json_object *arr = cfg ? keys_array(cfg, false) : NULL;
  for (size_t i = 0; arr && i < json_object_array_length(arr); i++) {
    struct json_object *e = json_object_array_get_idx(arr, i), *jk = NULL;
    if (!json_object_object_get_ex(e, "key", &jk)) continue;
    const char *k = json_object_get_string(jk);
    size_t klen = strlen(k);
    if (klen > SSH_KEY_MAX_LEN) {
      free(body); if (cfg) json_object_put(cfg);
      set_err(error, n, "stored key is too large"); return -EINVAL;
    }
    size_t need = len + klen + 2;
    if (need > cap) { while (cap < need) cap *= 2; char *p = realloc(body, cap);
      if (!p) { free(body); if (cfg) json_object_put(cfg); set_err(error, n, "out of memory"); return -ENOMEM; }
      body = p; }
    int printed = snprintf(body + len, cap - len, "%s\n", k);
    if (printed < 0 || (size_t)printed >= cap - len) {
      free(body); if (cfg) json_object_put(cfg);
      set_err(error, n, "key formatting failed"); return -EINVAL;
    }
    len += (size_t)printed;
  }
  if (cfg) json_object_put(cfg);

  int rc = write_atomic(file, body, 0600);
  free(body);
  if (rc != 0) set_err(error, n, strerror(-rc));
  return rc;
}

int ssh_keys_add(const char *label, const char *key, char *error, size_t n) {
  int rc = ssh_key_validate(key, error, n);
  if (rc != 0) return rc;
  char id[SSH_KEY_MAX_LEN];
  if (ssh_key_identity(key, id, sizeof(id)) != 0) {
    set_err(error, n, "key could not be normalized"); return -EINVAL;
  }
  struct json_object *cfg = load_config_file();
  if (!cfg) cfg = json_object_new_object();
  struct json_object *arr = keys_array(cfg, true);
  for (size_t i = 0; i < json_object_array_length(arr); i++) {
    struct json_object *e = json_object_array_get_idx(arr, i), *jk = NULL;
    char eid[SSH_KEY_MAX_LEN];
    if (json_object_object_get_ex(e, "key", &jk) &&
        ssh_key_identity(json_object_get_string(jk), eid, sizeof(eid)) == 0 &&
        !strcmp(eid, id)) {
      json_object_put(cfg);
      set_err(error, n, "an identical key is already authorized");
      return -EEXIST;
    }
  }
  struct json_object *entry = json_object_new_object();
  json_object_object_add(entry, "label",
      json_object_new_string(label ? label : ""));
  json_object_object_add(entry, "key", json_object_new_string(key));
  json_object_array_add(arr, entry);
  rc = save_config_file(cfg, error, n);
  json_object_put(cfg);
  if (rc != 0) return rc;
  return ssh_keys_render(error, n);
}

int ssh_keys_remove(const char *key, char *error, size_t n) {
  if (!key || !*key) { set_err(error, n, "key is required"); return -EINVAL; }
  struct json_object *cfg = load_config_file();
  if (!cfg) cfg = json_object_new_object();
  struct json_object *arr = keys_array(cfg, false);
  bool removed = false;
  for (size_t i = 0; arr && i < json_object_array_length(arr); i++) {
    struct json_object *e = json_object_array_get_idx(arr, i), *jk = NULL;
    if (json_object_object_get_ex(e, "key", &jk) &&
        !strcmp(json_object_get_string(jk), key)) {
      json_object_array_del_idx(arr, i, 1);
      removed = true;
      break;
    }
  }
  if (!removed) { json_object_put(cfg); set_err(error, n, "key not found"); return -ENOENT; }
  int rc = save_config_file(cfg, error, n);
  json_object_put(cfg);
  if (rc != 0) return rc;
  return ssh_keys_render(error, n);
}
