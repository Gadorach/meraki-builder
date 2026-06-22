#include "config_file.h"
#include "configd.h"
#include <libpostmerkos.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char runtime_error[256];

static int parent_directory(const char *path, char *directory, size_t size) {
  if (!path || !*path || !directory || !size) return -EINVAL;
  int length = snprintf(directory, size, "%s", path);
  if (length < 0 || (size_t)length >= size) return -ENAMETOOLONG;
  char *slash = strrchr(directory, '/');
  if (!slash) snprintf(directory, size, ".");
  else if (slash == directory) slash[1] = '\0';
  else *slash = '\0';
  return 0;
}

static void fsync_parent_directory(const char *path) {
  char directory[512];
  if (parent_directory(path, directory, sizeof(directory)) != 0) return;
  int fd = open(directory, O_RDONLY | O_DIRECTORY);
  if (fd < 0) return;
  if (fsync(fd) != 0 && errno != EINVAL && errno != EROFS)
    fprintf(stderr, "%s config: warning: directory fsync failed: %s\n",
            get_time(), strerror(errno));
  close(fd);
}

static int copy_file_atomic(const char *source, const char *destination,
                            mode_t mode, uid_t uid, gid_t gid) {
  char temp[640];
  int n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", destination, (long)getpid());
  if (n < 0 || (size_t)n >= sizeof(temp)) return -ENAMETOOLONG;
  int in = open(source, O_RDONLY);
  if (in < 0) return -errno;
  int out = open(temp, O_WRONLY | O_CREAT | O_EXCL, mode);
  if (out < 0) { int rc = -errno; close(in); return rc; }
  (void)fchown(out, uid, gid);
  char buffer[4096];
  int rc = 0;
  for (;;) {
    ssize_t got = read(in, buffer, sizeof(buffer));
    if (got == 0) break;
    if (got < 0) { if (errno == EINTR) continue; rc = -errno; break; }
    ssize_t off = 0;
    while (off < got) {
      ssize_t wrote = write(out, buffer + off, (size_t)(got - off));
      if (wrote < 0) { if (errno == EINTR) continue; rc = -errno; break; }
      off += wrote;
    }
    if (rc != 0) break;
  }
  if (rc == 0 && fsync(out) != 0) rc = -errno;
  close(in);
  if (close(out) != 0 && rc == 0) rc = -errno;
  if (rc == 0 && rename(temp, destination) != 0) rc = -errno;
  if (rc != 0) unlink(temp);
  else fsync_parent_directory(destination);
  return rc;
}

struct json_object *load_json_file(const char *path) {
  return path ? json_object_from_file(path) : NULL;
}

struct json_object *load_config_file(void) {
  return load_json_file(config_file);
}

static void set_error(char *error, size_t size, const char *message) {
  if (error && size) snprintf(error, size, "%s", message ? message : "error");
}

int save_config_file(struct json_object *json, char *error, size_t error_size) {
  if (!json) { set_error(error, error_size, "configuration is null"); return -EINVAL; }
  if (dry_run) return 0;

  char directory[512], lock_path[640], temp_path[640], backup_path[640];
  int rc = parent_directory(config_file, directory, sizeof(directory));
  if (rc != 0) { set_error(error, error_size, "configuration path is too long"); return rc; }
  if (snprintf(lock_path, sizeof(lock_path), "%s/.postmerkos-config.lock", directory) >= (int)sizeof(lock_path) ||
      snprintf(temp_path, sizeof(temp_path), "%s.tmp.%ld", config_file, (long)getpid()) >= (int)sizeof(temp_path) ||
      snprintf(backup_path, sizeof(backup_path), "%s.bak", config_file) >= (int)sizeof(backup_path)) {
    set_error(error, error_size, "configuration path is too long"); return -ENAMETOOLONG;
  }

  int lock_fd = open(lock_path, O_RDWR | O_CREAT, 0600);
  if (lock_fd < 0) { set_error(error, error_size, strerror(errno)); return -errno; }
  if (flock(lock_fd, LOCK_EX) != 0) {
    rc = -errno; close(lock_fd); set_error(error, error_size, strerror(-rc)); return rc;
  }

  mode_t mode = 0600; uid_t uid = getuid(); gid_t gid = getgid();
  struct stat existing;
  if (stat(config_file, &existing) == 0) {
    mode = existing.st_mode & 07777; uid = existing.st_uid; gid = existing.st_gid;
  }

  int fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL, mode);
  if (fd < 0) { rc = -errno; goto done; }
  (void)fchown(fd, uid, gid);
  const char *text = json_object_to_json_string_ext(
      json, JSON_C_TO_STRING_SPACED | JSON_C_TO_STRING_PRETTY);
  size_t remaining = strlen(text); const char *cursor = text;
  while (remaining) {
    ssize_t written = write(fd, cursor, remaining);
    if (written < 0) { if (errno == EINTR) continue; rc = -errno; break; }
    cursor += written; remaining -= (size_t)written;
  }
  if (rc == 0 && write(fd, "\n", 1) != 1) rc = -EIO;
  if (rc == 0 && fsync(fd) != 0) rc = -errno;
  if (close(fd) != 0 && rc == 0) rc = -errno;
  if (rc != 0) { unlink(temp_path); goto done; }
  if (rename(temp_path, config_file) != 0) { rc = -errno; unlink(temp_path); goto done; }
  fsync_parent_directory(config_file);

  /* The backup is copied from the newly committed, validated primary.  This
   * prevents a malformed externally edited file from replacing known-good. */
  rc = copy_file_atomic(config_file, backup_path, mode, uid, gid);
  if (rc != 0) {
    fprintf(stderr, "%s config: warning: known-good backup failed: %s\n",
            get_time(), strerror(-rc));
    rc = 0; /* primary commit remains valid */
  }

done:
  flock(lock_fd, LOCK_UN);
  close(lock_fd);
  if (rc != 0) { set_error(error, error_size, strerror(-rc)); return rc; }
  printf("%s config: atomically written to %s\n", get_time(), config_file);
  return 0;
}

int config_file_mtime(long long *mtime_ns) {
  if (!mtime_ns) return -EINVAL;
  struct stat st;
  if (stat(config_file, &st) != 0) return -errno;
#if defined(__linux__)
  *mtime_ns = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#else
  *mtime_ns = (long long)st.st_mtime * 1000000000LL;
#endif
  return 0;
}

void config_file_set_runtime_error(const char *message) {
  snprintf(runtime_error, sizeof(runtime_error), "%s", message ? message : "");
}

const char *config_file_runtime_error(void) {
  return runtime_error[0] ? runtime_error : NULL;
}
