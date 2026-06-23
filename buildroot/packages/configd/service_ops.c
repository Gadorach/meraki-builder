#define _GNU_SOURCE
#include "service_ops.h"
#include "time_ops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SERVICE_POLICY_PATH "/config/postmerkos/services.json"
#define SERVICE_POLICY_DEFAULT "/usr/share/postmerkos/defaults/services.json"

static void set_error(char *error, size_t size, const char *message) {
  if (error && size) snprintf(error, size, "%s", message ? message : "error");
}

static int ensure_parent(void) {
  mkdir("/config", 0755);
  mkdir("/config/postmerkos", 0700);
  return 0;
}

static int atomic_json_write(const char *path, struct json_object *object) {
  char temp[320];
  if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp))
    return -ENAMETOOLONG;
  int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -errno;
  const char *text = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PRETTY);
  size_t left = strlen(text);
  const char *cursor = text;
  int rc = 0;
  while (left) {
    ssize_t wrote = write(fd, cursor, left);
    if (wrote < 0) { if (errno == EINTR) continue; rc = -errno; break; }
    cursor += wrote; left -= (size_t)wrote;
  }
  if (!rc && write(fd, "\n", 1) != 1) rc = -EIO;
  if (!rc && fsync(fd) != 0) rc = -errno;
  if (close(fd) != 0 && !rc) rc = -errno;
  if (!rc && rename(temp, path) != 0) rc = -errno;
  if (rc) unlink(temp);
  return rc;
}

static struct json_object *member(struct json_object *object, const char *key) {
  struct json_object *value = NULL;
  if (!object || !json_object_is_type(object, json_type_object) ||
      !json_object_object_get_ex(object, key, &value)) return NULL;
  return value;
}

static bool bool_member(struct json_object *object, const char *key, bool fallback) {
  struct json_object *value = member(object, key);
  return value && json_object_is_type(value, json_type_boolean)
      ? json_object_get_boolean(value) : fallback;
}

static int int_member(struct json_object *object, const char *key, int fallback) {
  struct json_object *value = member(object, key);
  return value && json_object_is_type(value, json_type_int)
      ? json_object_get_int(value) : fallback;
}

static bool valid_service(struct json_object *service, bool ssh) {
  if (!service || !json_object_is_type(service, json_type_object)) return false;
  struct json_object *value = NULL;
  const char *booleans[] = {"enabled", "autostart", "password_auth"};
  size_t count = ssh ? 3 : 2;
  for (size_t i = 0; i < count; i++)
    if (json_object_object_get_ex(service, booleans[i], &value) &&
        !json_object_is_type(value, json_type_boolean)) return false;
  if (ssh && json_object_object_get_ex(service, "port", &value)) {
    if (!json_object_is_type(value, json_type_int)) return false;
    int port = json_object_get_int(value);
    if (port < 1 || port > 65535) return false;
  }
  return true;
}

static int validate_policy(struct json_object *policy, char *error, size_t error_size) {
  if (!policy || !json_object_is_type(policy, json_type_object)) {
    set_error(error, error_size, "service policy must be an object"); return -EINVAL;
  }
  struct json_object *ssh = member(policy, "ssh");
  struct json_object *web = member(policy, "web");
  struct json_object *chrony = member(policy, "chrony");
  if (!valid_service(ssh, true) || !valid_service(web, false) ||
      !valid_service(chrony, false)) {
    set_error(error, error_size, "service policy contains invalid fields"); return -EINVAL;
  }
  return 0;
}

struct json_object *service_policy_load(void) {
  struct json_object *policy = json_object_from_file(SERVICE_POLICY_PATH);
  if (!policy) policy = json_object_from_file(SERVICE_POLICY_DEFAULT);
  if (!policy) {
    policy = json_object_new_object();
    struct json_object *ssh = json_object_new_object();
    json_object_object_add(ssh, "enabled", json_object_new_boolean(true));
    json_object_object_add(ssh, "autostart", json_object_new_boolean(true));
    json_object_object_add(ssh, "password_auth", json_object_new_boolean(true));
    json_object_object_add(ssh, "port", json_object_new_int(22));
    json_object_object_add(policy, "ssh", ssh);
    const char *names[] = {"web", "chrony"};
    for (size_t i = 0; i < 2; i++) {
      struct json_object *entry = json_object_new_object();
      json_object_object_add(entry, "enabled", json_object_new_boolean(true));
      json_object_object_add(entry, "autostart", json_object_new_boolean(true));
      json_object_object_add(policy, names[i], entry);
    }
  }
  return policy;
}

static bool process_running(const char *name);

static const char *service_pattern(const char *service) {
  if (!strcmp(service, "ssh")) return "dropbear";
  if (!strcmp(service, "web")) return "uhttpd";
  if (!strcmp(service, "chrony")) return "chrony";
  if (!strcmp(service, "snmp")) return "snmpd";   /* matches S16snmpd init script */
  return NULL;
}

static const char *init_directory(void) {
  const char *path = getenv("POSTMERKOS_INIT_DIR");
  return path && *path ? path : "/etc/init.d";
}

static const char *proc_directory(void) {
  const char *path = getenv("POSTMERKOS_PROC_DIR");
  return path && *path ? path : "/proc";
}

static int find_init_script(const char *pattern, char *path, size_t path_size) {
  const char *base = init_directory();
  DIR *dir = opendir(base);
  if (!dir) return -errno;
  struct dirent *entry;
  int rc = -ENOENT;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] != 'S' || !strstr(entry->d_name, pattern)) continue;
    if (snprintf(path, path_size, "%s/%s", base, entry->d_name) >= (int)path_size)
      rc = -ENAMETOOLONG;
    else rc = 0;
    break;
  }
  closedir(dir);
  return rc;
}

static const char *service_process_name(const char *service) {
  if (!strcmp(service, "ssh")) return "dropbear";
  if (!strcmp(service, "web")) return "uhttpd";
  if (!strcmp(service, "chrony")) return "chronyd";
  if (!strcmp(service, "snmp")) return "mini-snmpd";
  return NULL;
}

static int run_script(const char *path, const char *action) {
  pid_t child = fork();
  if (child < 0) return -errno;
  if (child == 0) { execl(path, path, action, (char *)NULL); _exit(127); }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return -errno;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -EIO;
}

int service_action(const char *service, const char *action, char *error, size_t error_size) {
  const char *pattern = service ? service_pattern(service) : NULL;
  if (!pattern || !action || (strcmp(action, "start") && strcmp(action, "stop") &&
      strcmp(action, "restart"))) {
    set_error(error, error_size, "unknown service or action"); return -EINVAL;
  }
  const char *process = service_process_name(service);
  bool running = process && process_running(process);
  if ((!strcmp(action, "start") && running) ||
      (!strcmp(action, "stop") && !running)) return 0;

  char path[256];
  int rc = find_init_script(pattern, path, sizeof(path));
  if (rc != 0) { set_error(error, error_size, "service init script is not installed"); return rc; }
  rc = run_script(path, action);
  if (rc != 0) set_error(error, error_size, "service action failed");
  return rc;
}

static bool process_running(const char *name) {
  const char *base = proc_directory();
  DIR *proc = opendir(base);
  if (!proc) return false;
  struct dirent *entry;
  bool running = false;
  while ((entry = readdir(proc)) != NULL && !running) {
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
    char path[320], command[64] = {0};
    snprintf(path, sizeof(path), "%s/%s/comm", base, entry->d_name);
    FILE *file = fopen(path, "r");
    if (!file) continue;
    if (fgets(command, sizeof(command), file)) {
      command[strcspn(command, "\r\n")] = 0;
      if (!strcmp(command, name)) running = true;
    }
    fclose(file);
  }
  closedir(proc);
  return running;
}

static int write_dropbear_defaults(struct json_object *ssh) {
  FILE *file = fopen("/etc/default/dropbear", "w");
  if (!file) return -errno;
  int port = int_member(ssh, "port", 22);
  bool password = bool_member(ssh, "password_auth", true);
  fprintf(file, "DROPBEAR_PORT=%d\nDROPBEAR_EXTRA_ARGS='%s'\n", port,
          password ? "" : "-s");
  int rc = fclose(file) == 0 ? 0 : -errno;
  return rc;
}

static int service_policy_apply_object(struct json_object *policy,
                                       char *error, size_t error_size) {
  if (!policy) { set_error(error, error_size, "service policy unavailable"); return -ENOENT; }
  struct json_object *ssh = member(policy, "ssh");
  int rc = write_dropbear_defaults(ssh);
  if (rc == 0) rc = time_policy_apply(error, error_size);
  const char *names[] = {"ssh", "web", "chrony"};
  for (size_t i = 0; i < 3; i++) {
    struct json_object *entry = member(policy, names[i]);
    bool desired = bool_member(entry, "enabled", true) &&
                   bool_member(entry, "autostart", true);
    if (!strcmp(names[i], "chrony")) {
      struct json_object *time = time_policy_load();
      desired = desired && bool_member(time, "ntp_enabled", true);
      if (time) json_object_put(time);
    }
    char local_error[128] = {0};
    int action_rc = service_action(names[i], desired ? "start" : "stop",
                                   local_error, sizeof(local_error));
    if (action_rc != 0 && rc == 0) rc = action_rc;
  }
  if (rc != 0 && error && !*error)
    set_error(error, error_size, "one or more service settings could not be applied");
  return rc;
}

int service_policy_apply(char *error, size_t error_size) {
  struct json_object *policy = service_policy_load();
  if (!policy) { set_error(error, error_size, "service policy unavailable"); return -ENOENT; }
  int rc = service_policy_apply_object(policy, error, error_size);
  json_object_put(policy);
  return rc;
}

int service_policy_save(struct json_object *policy, char *error, size_t error_size) {
  int rc = validate_policy(policy, error, error_size);
  if (rc != 0) return rc;
  struct json_object *previous = service_policy_load();

  /* Apply before persist.  A desired policy is not committed unless its
   * observable service state can be established. */
  rc = service_policy_apply_object(policy, error, error_size);
  if (rc != 0) {
    char rollback_error[128] = {0};
    if (previous) service_policy_apply_object(previous, rollback_error,
                                              sizeof(rollback_error));
    if (previous) json_object_put(previous);
    return rc;
  }

  ensure_parent();
  rc = atomic_json_write(SERVICE_POLICY_PATH, policy);
  if (rc != 0) {
    char rollback_error[128] = {0};
    if (previous) service_policy_apply_object(previous, rollback_error,
                                              sizeof(rollback_error));
    if (previous) json_object_put(previous);
    set_error(error, error_size, strerror(-rc));
    return rc;
  }
  if (previous) json_object_put(previous);
  return 0;
}

struct json_object *service_status_json(void) {
  struct json_object *policy = service_policy_load();
  struct json_object *root = json_object_new_object();
  const char *names[] = {"ssh", "web", "chrony"};
  const char *processes[] = {"dropbear", "uhttpd", "chronyd"};
  for (size_t i = 0; i < 3; i++) {
    struct json_object *entry = member(policy, names[i]);
    struct json_object *status = json_object_new_object();
    bool desired = bool_member(entry, "enabled", true) &&
                   bool_member(entry, "autostart", true);
    if (!strcmp(names[i], "chrony")) {
      struct json_object *time = time_policy_load();
      desired = desired && bool_member(time, "ntp_enabled", true);
      if (time) json_object_put(time);
    }
    bool observed = process_running(processes[i]);
    json_object_object_add(status, "enabled", json_object_new_boolean(bool_member(entry, "enabled", true)));
    json_object_object_add(status, "autostart", json_object_new_boolean(bool_member(entry, "autostart", true)));
    json_object_object_add(status, "desired_running", json_object_new_boolean(desired));
    json_object_object_add(status, "running", json_object_new_boolean(observed));
    json_object_object_add(status, "in_sync", json_object_new_boolean(desired == observed));
    if (!strcmp(names[i], "ssh")) {
      json_object_object_add(status, "password_auth", json_object_new_boolean(bool_member(entry, "password_auth", true)));
      json_object_object_add(status, "port", json_object_new_int(int_member(entry, "port", 22)));
    }
    json_object_object_add(root, names[i], status);
  }
  json_object_put(policy);
  return root;
}
