#ifndef CONFIGD_SYSTEM_OPS_H
#define CONFIGD_SYSTEM_OPS_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>

struct json_object *terminal_execute(const char *command);
struct json_object *firmware_status_json(void);
int firmware_validate_update(const char *path, const char *artifact_name,
                             const char *manifest_path, const char *overlay,
                             bool force, bool accept_untested,
                             char *error, size_t error_size);
int firmware_start_update(const char *path, const char *artifact_name,
                          const char *manifest_path, const char *overlay,
                          bool force, bool accept_untested,
                          char *error, size_t error_size);
struct json_object *firmware_repositories_json(void);
int firmware_repositories_save(struct json_object *repositories,
                               char *error, size_t error_size);
struct json_object *firmware_repository_check(const char *source);

#endif
