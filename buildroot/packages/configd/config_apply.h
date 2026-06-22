#ifndef CONFIGD_CONFIG_APPLY_H
#define CONFIGD_CONFIG_APPLY_H

#include "result.h"
#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>

struct json_object *config_create_defaults(struct apply_result *result);
struct json_object *config_load_or_create(char *error, size_t error_size);
int config_apply_full(struct json_object *config, struct apply_result *result);
int config_apply_delta(struct json_object *full_config,
                       struct json_object *delta,
                       struct apply_result *result,
                       bool defer_network);
int config_merge_validate_save_apply(struct json_object *delta,
                                     struct json_object **saved_config,
                                     struct apply_result *result,
                                     bool defer_network,
                                     char *error, size_t error_size);
int config_replace_validate_save_apply(struct json_object *candidate,
                                       struct apply_result *result,
                                       char *error, size_t error_size);

#endif
