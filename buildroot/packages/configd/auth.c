#include "auth.h"
#include "roles.h"

#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define ACCOUNT_LINE_MAX 4096


static const char *account_path(const char *environment, const char *fallback) {
  const char *value = getenv(environment);
  return value && *value ? value : fallback;
}

struct account_record {
  char name[65];
  char password[512];
  uid_t uid;
  gid_t gid;
  char home[256];
  char shell[128];
};


static char *next_colon_field(char **cursor) {
  if (!cursor || !*cursor) return NULL;
  char *field = *cursor;
  char *separator = strchr(field, ':');
  if (separator) {
    *separator = '\0';
    *cursor = separator + 1;
  } else {
    *cursor = NULL;
  }
  return field;
}

static void set_error(char *error, size_t size, const char *message) {
  if (error && size) snprintf(error, size, "%s", message ? message : "error");
}

static bool safe_username(const char *username) {
  if (!username || !*username || strlen(username) > 64) return false;
  for (const unsigned char *p = (const unsigned char *)username; *p; p++) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
      return false;
  }
  return true;
}

static void trim_newline(char *text) {
  if (text) text[strcspn(text, "\r\n")] = '\0';
}

static bool parse_account_line(char *line, struct account_record *record) {
  char *cursor = line;
  char *name = next_colon_field(&cursor);
  char *password = next_colon_field(&cursor);
  char *uid_text = next_colon_field(&cursor);
  char *gid_text = next_colon_field(&cursor);
  (void)next_colon_field(&cursor); /* gecos */
  char *home = next_colon_field(&cursor);
  char *shell = next_colon_field(&cursor);
  if (!name || !password || !uid_text || !gid_text || !home || !shell)
    return false;
  char *end_uid = NULL, *end_gid = NULL;
  unsigned long uid = strtoul(uid_text, &end_uid, 10);
  unsigned long gid = strtoul(gid_text, &end_gid, 10);
  if (!end_uid || *end_uid || !end_gid || *end_gid) return false;
  memset(record, 0, sizeof(*record));
  snprintf(record->name, sizeof(record->name), "%s", name);
  snprintf(record->password, sizeof(record->password), "%s", password);
  record->uid = (uid_t)uid;
  record->gid = (gid_t)gid;
  snprintf(record->home, sizeof(record->home), "%s", home);
  snprintf(record->shell, sizeof(record->shell), "%s", shell);
  return true;
}

static int account_by_name(const char *username, struct account_record *record) {
  if (!safe_username(username) || !record) return -EINVAL;
  FILE *file = fopen(account_path("POSTMERKOS_PASSWD_FILE", "/etc/passwd"), "r");
  if (!file) return -errno;
  char line[ACCOUNT_LINE_MAX];
  int rc = -ENOENT;
  while (fgets(line, sizeof(line), file)) {
    trim_newline(line);
    struct account_record candidate;
    if (!parse_account_line(line, &candidate)) continue;
    if (!strcmp(candidate.name, username)) {
      *record = candidate;
      rc = 0;
      break;
    }
  }
  fclose(file);
  return rc;
}

static bool account_exists(const char *username) {
  struct account_record record;
  return account_by_name(username, &record) == 0;
}

static int shadow_hash(const char *username, char *hash, size_t hash_size) {
  FILE *file = fopen(account_path("POSTMERKOS_SHADOW_FILE", "/etc/shadow"), "r");
  if (!file) return -errno;
  char line[ACCOUNT_LINE_MAX];
  int rc = -ENOENT;
  while (fgets(line, sizeof(line), file)) {
    trim_newline(line);
    char *cursor = line;
    char *name = next_colon_field(&cursor);
    char *password = next_colon_field(&cursor);
    if (!name || !password || strcmp(name, username)) continue;
    if (snprintf(hash, hash_size, "%s", password) >= (int)hash_size)
      rc = -ENAMETOOLONG;
    else
      rc = 0;
    break;
  }
  fclose(file);
  return rc;
}

static bool constant_time_equal(const char *left, const char *right) {
  if (!left || !right) return false;
  size_t left_length = strlen(left);
  size_t right_length = strlen(right);
  size_t length = left_length > right_length ? left_length : right_length;
  unsigned int difference = (unsigned int)(left_length ^ right_length);
  for (size_t i = 0; i < length; i++) {
    unsigned char a = i < left_length ? (unsigned char)left[i] : 0;
    unsigned char b = i < right_length ? (unsigned char)right[i] : 0;
    difference |= (unsigned int)(a ^ b);
  }
  return difference == 0;
}

int auth_verify_user(const char *username, const char *password,
                     char *error, size_t error_size) {
  if (!safe_username(username) || !password) {
    set_error(error, error_size, "invalid credentials");
    return -EINVAL;
  }
  struct account_record account;
  if (account_by_name(username, &account) != 0 ||
      role_for_username(username) == POSTMERKOS_ROLE_NONE) {
    set_error(error, error_size,
              "invalid credentials or account is not authorized for switch management");
    return -EACCES;
  }
  char hash[512];
  if (account.password[0] && strcmp(account.password, "x") &&
      strcmp(account.password, "*"))
    snprintf(hash, sizeof(hash), "%s", account.password);
  else if (shadow_hash(account.name, hash, sizeof(hash)) != 0) {
    set_error(error, error_size, "account password is unavailable");
    return -EACCES;
  }
  if (!hash[0] || hash[0] == '!' || hash[0] == '*') {
    set_error(error, error_size, "account password is locked or unavailable");
    return -EACCES;
  }
  char *calculated = crypt(password, hash);
  if (!calculated || !constant_time_equal(calculated, hash)) {
    set_error(error, error_size, "authentication failed");
    return -EACCES;
  }
  return 0;
}

struct json_object *auth_list_users(void) {
  struct json_object *users = json_object_new_array();
  FILE *file = fopen(account_path("POSTMERKOS_PASSWD_FILE", "/etc/passwd"), "r");
  if (!file) return users;
  char line[ACCOUNT_LINE_MAX];
  while (fgets(line, sizeof(line), file)) {
    trim_newline(line);
    struct account_record entry;
    if (!parse_account_line(line, &entry) || !safe_username(entry.name)) continue;
    enum postmerkos_role role = entry.uid == 0
        ? POSTMERKOS_ROLE_ADMIN : role_for_username(entry.name);
    if (role == POSTMERKOS_ROLE_NONE || strstr(entry.shell, "nologin") ||
        strstr(entry.shell, "false")) continue;
    struct json_object *user = json_object_new_object();
    json_object_object_add(user, "username", json_object_new_string(entry.name));
    json_object_object_add(user, "uid", json_object_new_int64((int64_t)entry.uid));
    json_object_object_add(user, "home", json_object_new_string(entry.home));
    json_object_object_add(user, "shell", json_object_new_string(entry.shell));
    json_object_object_add(user, "role", json_object_new_string(role_name(role)));
    json_object_object_add(user, "capabilities", role_capabilities_json(role));
    json_object_array_add(users, user);
  }
  fclose(file);
  return users;
}

static int run_chpasswd(const char *username, const char *new_password,
                        char *error, size_t error_size) {
  int input_pipe[2];
  if (pipe(input_pipe) != 0) {
    set_error(error, error_size, strerror(errno));
    return -errno;
  }
  pid_t child = fork();
  if (child < 0) {
    int saved = errno;
    close(input_pipe[0]);
    close(input_pipe[1]);
    set_error(error, error_size, strerror(saved));
    return -saved;
  }
  if (child == 0) {
    dup2(input_pipe[0], STDIN_FILENO);
    int nullfd = open("/dev/null", O_WRONLY);
    if (nullfd >= 0) {
      dup2(nullfd, STDOUT_FILENO);
      dup2(nullfd, STDERR_FILENO);
      if (nullfd > STDERR_FILENO) close(nullfd);
    }
    close(input_pipe[0]);
    close(input_pipe[1]);
    setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
    execlp("chpasswd", "chpasswd", (char *)NULL);
    _exit(127);
  }
  close(input_pipe[0]);
  dprintf(input_pipe[1], "%s:%s\n", username, new_password);
  close(input_pipe[1]);
  int status = 0;
  if (waitpid(child, &status, 0) < 0) {
    set_error(error, error_size, strerror(errno));
    return -errno;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    set_error(error, error_size, "password update failed");
    return -EIO;
  }
  return 0;
}

int auth_change_password(const char *actor, const char *target,
                         const char *actor_password,
                         const char *new_password,
                         char *error, size_t error_size) {
  struct account_record actor_entry, target_entry;
  if (!safe_username(actor) || !safe_username(target) ||
      account_by_name(target, &target_entry) != 0 ||
      role_for_username(target) == POSTMERKOS_ROLE_NONE) {
    set_error(error, error_size, "unknown or unauthorized target account");
    return -ENOENT;
  }
  if (!new_password || strlen(new_password) < 8 || strlen(new_password) > 128 ||
      strchr(new_password, '\n') || strchr(new_password, '\r') ||
      strchr(new_password, ':')) {
    set_error(error, error_size,
              "new password must be 8-128 characters and may not contain ':' or line breaks");
    return -EINVAL;
  }
  int rc = auth_verify_user(actor, actor_password, error, error_size);
  if (rc != 0) return rc;
  if (account_by_name(actor, &actor_entry) != 0) return -ENOENT;
  if (actor_entry.uid != 0 && strcmp(actor, target)) {
    set_error(error, error_size, "only root may change another account");
    return -EACCES;
  }
  rc = run_chpasswd(target, new_password, error, error_size);
  if (rc == 0 && !strcmp(target, "root"))
    unlink("/config/postmerkos/default-password-active");
  return rc;
}

static int run_command(char *const argv[], char *error, size_t error_size) {
  pid_t child = fork();
  if (child < 0) { set_error(error, error_size, strerror(errno)); return -errno; }
  if (child == 0) {
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd >= 0) {
      dup2(nullfd, STDIN_FILENO); dup2(nullfd, STDOUT_FILENO); dup2(nullfd, STDERR_FILENO);
      if (nullfd > STDERR_FILENO) close(nullfd);
    }
    setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
    execvp(argv[0], argv); _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0)
    if (errno != EINTR) { set_error(error, error_size, strerror(errno)); return -errno; }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    set_error(error, error_size, "account operation failed"); return -EIO;
  }
  return 0;
}

static const char *role_group(const char *role) {
  if (role && !strcmp(role, "admin")) return "postmerkos-admin";
  if (role && !strcmp(role, "operator")) return "postmerkos-operator";
  if (role && !strcmp(role, "viewer")) return "postmerkos-viewer";
  return NULL;
}

static bool group_exists(const char *group) {
  FILE *file = fopen(account_path("POSTMERKOS_GROUP_FILE", "/etc/group"), "r");
  if (!file) return false;
  char line[ACCOUNT_LINE_MAX];
  bool exists = false;
  while (fgets(line, sizeof(line), file)) {
    char *colon = strchr(line, ':');
    if (colon) *colon = '\0';
    if (!strcmp(line, group)) { exists = true; break; }
  }
  fclose(file);
  return exists;
}

static bool group_has_member(const char *group, const char *username) {
  FILE *file = fopen(account_path("POSTMERKOS_GROUP_FILE", "/etc/group"), "r");
  if (!file) return false;
  char line[ACCOUNT_LINE_MAX];
  bool member = false;
  while (fgets(line, sizeof(line), file)) {
    trim_newline(line);
    char *cursor = line;
    char *name = next_colon_field(&cursor);
    (void)next_colon_field(&cursor);
    (void)next_colon_field(&cursor);
    char *members = next_colon_field(&cursor);
    if (!name || strcmp(name, group)) continue;
    char *member_save = NULL;
    for (char *entry = members ? strtok_r(members, ",", &member_save) : NULL;
         entry; entry = strtok_r(NULL, ",", &member_save))
      if (!strcmp(entry, username)) { member = true; break; }
    break;
  }
  fclose(file);
  return member;
}

static int ensure_group(const char *group, char *error, size_t error_size) {
  if (group_exists(group)) return 0;
  char *args[] = {"addgroup", (char *)group, NULL};
  return run_command(args, error, error_size);
}

static int group_membership(const char *username, const char *group, bool add,
                            char *error, size_t error_size) {
  char *add_args[] = {"addgroup", (char *)username, (char *)group, NULL};
  char *remove_args[] = {"delgroup", (char *)username, (char *)group, NULL};
  return run_command(add ? add_args : remove_args, error, error_size);
}

int auth_set_role(const char *username, const char *role,
                  char *error, size_t error_size) {
  if (!safe_username(username) || !account_exists(username)) {
    set_error(error, error_size, "unknown user"); return -ENOENT;
  }
  if (!strcmp(username, "root")) {
    if (!role || strcmp(role, "admin")) {
      set_error(error, error_size, "root must remain an administrator"); return -EPERM;
    }
    return 0;
  }
  const char *target = role_group(role);
  if (!target) {
    set_error(error, error_size, "role must be admin, operator, or viewer"); return -EINVAL;
  }
  int rc = ensure_group(target, error, error_size); if (rc != 0) return rc;
  const char *groups[] = {"postmerkos-admin", "postmerkos-operator", "postmerkos-viewer"};
  for (size_t i = 0; i < 3; i++) {
    bool member = group_has_member(groups[i], username);
    if (!strcmp(groups[i], target)) {
      if (!member && group_membership(username, groups[i], true, error, error_size) != 0)
        return -EIO;
    } else if (member) {
      char ignored[64] = {0};
      group_membership(username, groups[i], false, ignored, sizeof(ignored));
    }
  }
  return 0;
}

int auth_create_user(const char *username, const char *password,
                     const char *role, char *error, size_t error_size) {
  if (!safe_username(username) || account_exists(username)) {
    set_error(error, error_size, "username is invalid or already exists"); return -EINVAL;
  }
  if (!password || strlen(password) < 8 || strlen(password) > 128 ||
      strchr(password, ':') || strchr(password, '\n') || strchr(password, '\r')) {
    set_error(error, error_size,
              "password must be 8-128 characters without ':' or line breaks"); return -EINVAL;
  }
  if (!role_group(role)) {
    set_error(error, error_size, "role must be admin, operator, or viewer"); return -EINVAL;
  }
  char *args[] = {"adduser", "-D", "-s", "/bin/sh", (char *)username, NULL};
  int rc = run_command(args, error, error_size); if (rc != 0) return rc;
  rc = run_chpasswd(username, password, error, error_size);
  if (rc == 0) rc = auth_set_role(username, role, error, error_size);
  if (rc != 0) {
    char *del[] = {"deluser", (char *)username, NULL};
    char ignored[64]; run_command(del, ignored, sizeof(ignored));
  }
  return rc;
}

int auth_delete_user(const char *username, char *error, size_t error_size) {
  if (!safe_username(username) || !account_exists(username)) {
    set_error(error, error_size, "unknown user"); return -ENOENT;
  }
  if (!strcmp(username, "root")) {
    set_error(error, error_size, "root cannot be deleted"); return -EPERM;
  }
  char *args[] = {"deluser", (char *)username, NULL};
  return run_command(args, error, error_size);
}
