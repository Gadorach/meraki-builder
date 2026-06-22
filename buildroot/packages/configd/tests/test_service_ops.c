#define _GNU_SOURCE
#include "service_ops.h"
#include "time_ops.h"

#include <assert.h>
#include <errno.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int time_policy_apply(char *error, size_t error_size) {
  (void)error; (void)error_size; return 0;
}
struct json_object *time_policy_load(void) {
  return json_tokener_parse("{\"ntp_enabled\":true}");
}

static void write_file(const char *path, const char *text, mode_t mode) {
  FILE *file = fopen(path, "w");
  assert(file);
  assert(fputs(text, file) >= 0);
  assert(fclose(file) == 0);
  assert(chmod(path, mode) == 0);
}

int main(void) {
  char root[] = "/tmp/configd-service-test-XXXXXX";
  assert(mkdtemp(root));
  char init[PATH_MAX], proc[PATH_MAX], process[PATH_MAX];
  char comm[PATH_MAX], script[PATH_MAX], log[PATH_MAX];
  snprintf(init, sizeof(init), "%s/init", root);
  snprintf(proc, sizeof(proc), "%s/proc", root);
  snprintf(process, sizeof(process), "%s/proc/100", root);
  snprintf(comm, sizeof(comm), "%s/proc/100/comm", root);
  snprintf(script, sizeof(script), "%s/init/S50chrony", root);
  snprintf(log, sizeof(log), "%s/actions", root);
  assert(mkdir(init, 0700) == 0);
  assert(mkdir(proc, 0700) == 0);
  assert(mkdir(process, 0700) == 0);
  write_file(comm, "chronyd\n", 0600);
  write_file(script, "#!/bin/sh\nprintf '%s\\n' \"$1\" >>\"$SERVICE_ACTION_LOG\"\n", 0700);
  assert(setenv("POSTMERKOS_INIT_DIR", init, 1) == 0);
  assert(setenv("POSTMERKOS_PROC_DIR", proc, 1) == 0);
  assert(setenv("SERVICE_ACTION_LOG", log, 1) == 0);

  char error[128] = {0};
  assert(service_action("chrony", "start", error, sizeof(error)) == 0);
  assert(access(log, F_OK) != 0);
  assert(service_action("chrony", "stop", error, sizeof(error)) == 0);
  FILE *file = fopen(log, "r");
  assert(file);
  char action[32] = {0};
  assert(fgets(action, sizeof(action), file));
  fclose(file);
  assert(strcmp(action, "stop\n") == 0);

  assert(unlink(comm) == 0);
  assert(rmdir(process) == 0);
  assert(unlink(log) == 0);
  assert(service_action("chrony", "start", error, sizeof(error)) == 0);
  file = fopen(log, "r");
  assert(file && fgets(action, sizeof(action), file));
  fclose(file);
  assert(strcmp(action, "start\n") == 0);

  puts("configd idempotent service action tests passed");
  return 0;
}
