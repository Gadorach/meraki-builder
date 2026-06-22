#ifndef CONFIGD_ROLES_H
#define CONFIGD_ROLES_H
#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

enum postmerkos_role {
  POSTMERKOS_ROLE_NONE = 0,
  POSTMERKOS_ROLE_VIEWER,
  POSTMERKOS_ROLE_OPERATOR,
  POSTMERKOS_ROLE_ADMIN
};

int account_username_for_uid(uid_t uid, char *username, size_t username_size);
int account_uid_for_username(const char *username, uid_t *uid, gid_t *gid);
enum postmerkos_role role_for_username(const char *username);
enum postmerkos_role role_for_uid(uid_t uid);
const char *role_name(enum postmerkos_role role);
bool role_has_capability(enum postmerkos_role role, const char *capability);
struct json_object *role_capabilities_json(enum postmerkos_role role);
struct json_object *role_identity_json(const char *username);
struct json_object *role_identity_json_for_uid(uid_t uid);
#endif
