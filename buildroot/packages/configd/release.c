#include "release.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char cached_version[64];
static char cached_compatibility[40];
static char cached_project_repo[160];

struct json_object *release_info_load(void) {
  const char *path = getenv("POSTMERKOS_RELEASE_FILE");
  if (!path || !*path) path = "/etc/postmerkos-release.json";
  struct json_object *release = json_object_from_file(path);
  if (!release || !json_object_is_type(release, json_type_object)) {
    if (release) json_object_put(release);
    release = json_object_new_object();
    json_object_object_add(release, "version", json_object_new_string("unknown"));
    json_object_object_add(release, "metadata_present", json_object_new_boolean(0));
    return release;
  }
  json_object_object_add(release, "metadata_present", json_object_new_boolean(1));
  return release;
}

const char *release_version(void) {
  struct json_object *release = release_info_load();
  struct json_object *value = NULL;
  const char *version = "unknown";
  if (json_object_object_get_ex(release, "version", &value) &&
      json_object_is_type(value, json_type_string))
    version = json_object_get_string(value);
  snprintf(cached_version, sizeof(cached_version), "%s", version);
  json_object_put(release);
  return cached_version;
}

const char *release_project_repo(void) {
  struct json_object *release = release_info_load();
  struct json_object *value = NULL;
  const char *repo = "";
  if (json_object_object_get_ex(release, "project_repo", &value) &&
      json_object_is_type(value, json_type_string))
    repo = json_object_get_string(value);
  snprintf(cached_project_repo, sizeof(cached_project_repo), "%s", repo);
  json_object_put(release);
  return cached_project_repo;
}


const char *release_model_compatibility(const char *model) {
  cached_compatibility[0] = '\0';
  if (!model || !*model) return NULL;
  struct json_object *release = release_info_load();
  struct json_object *models = NULL;
  struct json_object *state = NULL;
  if (json_object_object_get_ex(release, "models", &models) &&
      json_object_is_type(models, json_type_object) &&
      json_object_object_get_ex(models, model, &state) &&
      json_object_is_type(state, json_type_string)) {
    snprintf(cached_compatibility, sizeof(cached_compatibility), "%s",
             json_object_get_string(state));
  }
  json_object_put(release);
  return cached_compatibility[0] ? cached_compatibility : NULL;
}
