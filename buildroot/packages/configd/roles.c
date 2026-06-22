#include "roles.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACCOUNT_LINE_MAX 4096
#define ACCOUNT_NAME_MAX 128

static const char *account_path(const char *environment, const char *fallback) {
  const char *value = getenv(environment);
  return value && *value ? value : fallback;
}


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

static void trim_newline(char *text) {
  if (text) text[strcspn(text, "\r\n")] = '\0';
}

/* Parse the account files directly instead of keeping pointers returned by
 * getpwnam/getpwuid/getgrnam.  Several libc implementations, including the
 * small target libc used by postmerkOS, reuse static storage for those calls.
 * Nested user/group lookups could therefore overwrite the username while a
 * role was being resolved. */
int account_uid_for_username(const char *username, uid_t *uid, gid_t *gid) {
  if (!username || !*username) return -EINVAL;
  FILE *file = fopen(account_path("POSTMERKOS_PASSWD_FILE", "/etc/passwd"), "r");
  if (!file) return -errno;
  char line[ACCOUNT_LINE_MAX];
  int rc = -ENOENT;
  while (fgets(line, sizeof(line), file)) {
    trim_newline(line);
    char *cursor = line;
    char *name = next_colon_field(&cursor);
    (void)next_colon_field(&cursor); /* password */
    char *uid_text = next_colon_field(&cursor);
    char *gid_text = next_colon_field(&cursor);
    if (!name || !uid_text || !gid_text || strcmp(name, username)) continue;
    char *end_uid = NULL, *end_gid = NULL;
    unsigned long parsed_uid = strtoul(uid_text, &end_uid, 10);
    unsigned long parsed_gid = strtoul(gid_text, &end_gid, 10);
    if (!end_uid || *end_uid || !end_gid || *end_gid) { rc = -EINVAL; break; }
    if (uid) *uid = (uid_t)parsed_uid;
    if (gid) *gid = (gid_t)parsed_gid;
    rc = 0;
    break;
  }
  fclose(file);
  return rc;
}

int account_username_for_uid(uid_t uid, char *username, size_t username_size) {
  if (!username || username_size == 0) return -EINVAL;
  username[0] = '\0';
  if (uid == 0) {
    snprintf(username, username_size, "root");
    return 0;
  }
  FILE *file = fopen(account_path("POSTMERKOS_PASSWD_FILE", "/etc/passwd"), "r");
  if (!file) return -errno;
  char line[ACCOUNT_LINE_MAX];
  int rc = -ENOENT;
  while (fgets(line, sizeof(line), file)) {
    trim_newline(line);
    char *cursor = line;
    char *name = next_colon_field(&cursor);
    (void)next_colon_field(&cursor);
    char *uid_text = next_colon_field(&cursor);
    if (!name || !uid_text) continue;
    char *end = NULL;
    unsigned long parsed = strtoul(uid_text, &end, 10);
    if (!end || *end || (uid_t)parsed != uid) continue;
    if (snprintf(username, username_size, "%s", name) >= (int)username_size)
      rc = -ENAMETOOLONG;
    else
      rc = 0;
    break;
  }
  fclose(file);
  return rc;
}

static bool member_list_contains(const char *members, const char *username) {
  if (!members || !username) return false;
  char copy[ACCOUNT_LINE_MAX];
  if (snprintf(copy, sizeof(copy), "%s", members) >= (int)sizeof(copy))
    return false;
  char *save = NULL;
  for (char *member = strtok_r(copy, ",", &save); member;
       member = strtok_r(NULL, ",", &save))
    if (!strcmp(member, username)) return true;
  return false;
}

static bool group_contains(const char *username, gid_t primary_gid,
                           const char *group_name) {
  FILE *file = fopen(account_path("POSTMERKOS_GROUP_FILE", "/etc/group"), "r");
  if (!file) return false;
  char line[ACCOUNT_LINE_MAX];
  bool found = false;
  while (fgets(line, sizeof(line), file)) {
    trim_newline(line);
    char *cursor = line;
    char *name = next_colon_field(&cursor);
    (void)next_colon_field(&cursor); /* password */
    char *gid_text = next_colon_field(&cursor);
    char *members = next_colon_field(&cursor);
    if (!name || !gid_text || strcmp(name, group_name)) continue;
    char *end = NULL;
    unsigned long parsed_gid = strtoul(gid_text, &end, 10);
    if (end && !*end && (gid_t)parsed_gid == primary_gid) found = true;
    if (!found && members && member_list_contains(members, username)) found = true;
    break;
  }
  fclose(file);
  return found;
}

enum postmerkos_role role_for_username(const char *username) {
  if (!username || !*username) return POSTMERKOS_ROLE_NONE;
  uid_t uid = (uid_t)-1;
  gid_t gid = (gid_t)-1;
  if (account_uid_for_username(username, &uid, &gid) != 0)
    return POSTMERKOS_ROLE_NONE;
  if (uid == 0) return POSTMERKOS_ROLE_ADMIN;
  if (group_contains(username, gid, "postmerkos-admin"))
    return POSTMERKOS_ROLE_ADMIN;
  if (group_contains(username, gid, "postmerkos-operator"))
    return POSTMERKOS_ROLE_OPERATOR;
  if (group_contains(username, gid, "postmerkos-viewer"))
    return POSTMERKOS_ROLE_VIEWER;
  return POSTMERKOS_ROLE_NONE;
}

enum postmerkos_role role_for_uid(uid_t uid) {
  if (uid == 0) return POSTMERKOS_ROLE_ADMIN;
  char username[ACCOUNT_NAME_MAX];
  return account_username_for_uid(uid, username, sizeof(username)) == 0
      ? role_for_username(username) : POSTMERKOS_ROLE_NONE;
}

const char *role_name(enum postmerkos_role role) {
  switch (role) {
    case POSTMERKOS_ROLE_ADMIN: return "administrator";
    case POSTMERKOS_ROLE_OPERATOR: return "operator";
    case POSTMERKOS_ROLE_VIEWER: return "viewer";
    default: return "none";
  }
}

static const char *viewer_caps[] = {
  "status.read", "config.read", "firmware.history.read", NULL
};
static const char *operator_caps[] = {
  "status.read", "config.read", "firmware.history.read", "ports.write",
  "switching.write", "backup.create", "system.reboot", NULL
};
static const char *admin_caps[] = {
  "status.read", "config.read", "firmware.history.read", "ports.write",
  "switching.write", "backup.create", "system.reboot", "system.poweroff",
  "firmware.update", "config.restore", "network.write", "users.manage",
  "services.manage", "terminal.exec", "system.factory_reset", NULL
};

static const char *const *capabilities(enum postmerkos_role role) {
  if (role == POSTMERKOS_ROLE_ADMIN) return admin_caps;
  if (role == POSTMERKOS_ROLE_OPERATOR) return operator_caps;
  if (role == POSTMERKOS_ROLE_VIEWER) return viewer_caps;
  return NULL;
}

bool role_has_capability(enum postmerkos_role role, const char *capability) {
  if (!capability) return false;
  const char *const *list = capabilities(role);
  for (size_t i = 0; list && list[i]; i++)
    if (!strcmp(list[i], capability)) return true;
  return false;
}

struct json_object *role_capabilities_json(enum postmerkos_role role) {
  struct json_object *array = json_object_new_array();
  const char *const *list = capabilities(role);
  for (size_t i = 0; list && list[i]; i++)
    json_object_array_add(array, json_object_new_string(list[i]));
  return array;
}

static struct json_object *identity_json(const char *username, uid_t uid,
                                         enum postmerkos_role role) {
  struct json_object *identity = json_object_new_object();
  json_object_object_add(identity, "username",
      json_object_new_string(username ? username : ""));
  json_object_object_add(identity, "uid", json_object_new_int64((int64_t)uid));
  json_object_object_add(identity, "role", json_object_new_string(role_name(role)));
  json_object_object_add(identity, "capabilities", role_capabilities_json(role));
  return identity;
}

struct json_object *role_identity_json(const char *username) {
  uid_t uid = (uid_t)-1;
  gid_t gid = (gid_t)-1;
  enum postmerkos_role role = POSTMERKOS_ROLE_NONE;
  if (username && account_uid_for_username(username, &uid, &gid) == 0)
    role = uid == 0 ? POSTMERKOS_ROLE_ADMIN : role_for_username(username);
  return identity_json(username, uid, role);
}

struct json_object *role_identity_json_for_uid(uid_t uid) {
  char username[ACCOUNT_NAME_MAX] = "";
  if (account_username_for_uid(uid, username, sizeof(username)) != 0)
    snprintf(username, sizeof(username), "uid-%lu", (unsigned long)uid);
  return identity_json(username, uid, role_for_uid(uid));
}
