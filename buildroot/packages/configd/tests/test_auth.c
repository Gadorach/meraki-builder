#include "auth.h"
#include <assert.h>
#include <crypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  assert(argc == 4);
  setenv("POSTMERKOS_PASSWD_FILE", argv[1], 1);
  setenv("POSTMERKOS_GROUP_FILE", argv[2], 1);
  setenv("POSTMERKOS_SHADOW_FILE", argv[3], 1);

  char *root_hash = crypt("root-password", "$6$root-test$");
  assert(root_hash);
  char root_copy[512];
  snprintf(root_copy, sizeof(root_copy), "%s", root_hash);
  char *bob_hash = crypt("operator-password", "$6$operator-test$");
  assert(bob_hash);
  char bob_copy[512];
  snprintf(bob_copy, sizeof(bob_copy), "%s", bob_hash);

  FILE *shadow = fopen(argv[3], "w");
  assert(shadow);
  fprintf(shadow, "root:%s:0:0:99999:7:::\n", root_copy);
  fprintf(shadow, "bob:%s:0:0:99999:7:::\n", bob_copy);
  fprintf(shadow, "nobody:!:0:0:99999:7:::\n");
  fclose(shadow);

  char error[256];
  for (int i = 0; i < 500; i++) {
    memset(error, 0, sizeof(error));
    assert(auth_verify_user("root", "root-password", error, sizeof(error)) == 0);
    memset(error, 0, sizeof(error));
    assert(auth_verify_user("bob", "operator-password", error, sizeof(error)) == 0);
  }
  assert(auth_verify_user("root", "wrong", error, sizeof(error)) != 0);
  assert(auth_verify_user("nobody", "anything", error, sizeof(error)) != 0);
  puts("authentication and role repeatability tests passed");
  return 0;
}
