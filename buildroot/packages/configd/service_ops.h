#ifndef CONFIGD_SERVICE_OPS_H
#define CONFIGD_SERVICE_OPS_H
#include <json-c/json.h>
#include <stddef.h>

struct json_object *service_policy_load(void);
struct json_object *service_status_json(void);
int service_policy_save(struct json_object *policy, char *error, size_t error_size);
int service_action(const char *service, const char *action, char *error, size_t error_size);
int service_policy_apply(char *error, size_t error_size);
int service_reconfigure(const char *service, char *error, size_t error_size);
int service_reconfigure_deferred(const char *service, int delay_ms,
                                 char *error, size_t error_size);
#endif
