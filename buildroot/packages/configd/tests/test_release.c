#include "release.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_file(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  assert(f);
  fputs(content, f);
  fclose(f);
}

int main(void) {
  char path[] = "/tmp/postmerkos-release-test-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  setenv("POSTMERKOS_RELEASE_FILE", path, 1);

  write_file(path, "{\"version\":\"x\",\"project_repo\":\"Owner/Repo\"}");
  assert(strcmp(release_project_repo(), "Owner/Repo") == 0);

  write_file(path, "{\"version\":\"x\"}");
  assert(strcmp(release_project_repo(), "") == 0);

  unlink(path);
  printf("test_release: OK\n");
  return 0;
}
