#include "result.h"

#include <stdio.h>
#include <string.h>

static void append_message(struct json_object *array, const char *format,
                           va_list args) {
  if (!array || !format) return;
  char buffer[256];
  vsnprintf(buffer, sizeof(buffer), format, args);
  json_object_array_add(array, json_object_new_string(buffer));
}

void apply_result_init(struct apply_result *result) {
  if (!result) return;
  result->warnings = json_object_new_array();
  result->failures = json_object_new_array();
  result->pending = json_object_new_array();
  result->unsupported = json_object_new_array();
  result->applied = 0;
  result->required_failed = false;
  result->runtime_degraded = false;
}

void apply_result_cleanup(struct apply_result *result) {
  if (!result) return;
  if (result->warnings) json_object_put(result->warnings);
  if (result->failures) json_object_put(result->failures);
  if (result->pending) json_object_put(result->pending);
  if (result->unsupported) json_object_put(result->unsupported);
  memset(result, 0, sizeof(*result));
}

void apply_result_warn(struct apply_result *result, const char *format, ...) {
  if (!result) return;
  va_list args; va_start(args, format);
  append_message(result->warnings, format, args);
  va_end(args);
  result->runtime_degraded = true;
}

void apply_result_fail(struct apply_result *result, const char *format, ...) {
  if (!result) return;
  va_list args; va_start(args, format);
  append_message(result->failures, format, args);
  va_end(args);
  result->required_failed = true;
  result->runtime_degraded = true;
}

void apply_result_pending(struct apply_result *result, const char *format, ...) {
  if (!result) return;
  va_list args; va_start(args, format);
  append_message(result->pending, format, args);
  va_end(args);
}

void apply_result_unsupported(struct apply_result *result, const char *format, ...) {
  if (!result) return;
  va_list args; va_start(args, format);
  append_message(result->unsupported, format, args);
  va_end(args);
}

void apply_result_applied(struct apply_result *result) {
  if (result) result->applied++;
}

bool apply_result_success(const struct apply_result *result) {
  return !result || !result->required_failed;
}

struct json_object *apply_result_json(const struct apply_result *result,
                                      const char *message) {
  struct json_object *data = json_object_new_object();
  json_object_object_add(data, "message",
      json_object_new_string(message ? message : "Configuration accepted"));
  json_object_object_add(data, "applied",
      json_object_new_int(result ? (int)result->applied : 0));
  json_object_object_add(data, "success",
      json_object_new_boolean(apply_result_success(result)));
  json_object_object_add(data, "runtime_degraded",
      json_object_new_boolean(result && result->runtime_degraded));
  json_object_object_add(data, "warnings", result && result->warnings
      ? json_object_get(result->warnings) : json_object_new_array());
  json_object_object_add(data, "failed", result && result->failures
      ? json_object_get(result->failures) : json_object_new_array());
  json_object_object_add(data, "pending", result && result->pending
      ? json_object_get(result->pending) : json_object_new_array());
  json_object_object_add(data, "unsupported", result && result->unsupported
      ? json_object_get(result->unsupported) : json_object_new_array());
  return data;
}
