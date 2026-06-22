#include "roles.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  assert(argc == 3);
  setenv("POSTMERKOS_PASSWD_FILE", argv[1], 1);
  setenv("POSTMERKOS_GROUP_FILE", argv[2], 1);
  assert(role_for_uid(0) == POSTMERKOS_ROLE_ADMIN);
  assert(role_for_username("root") == POSTMERKOS_ROLE_ADMIN);
  assert(role_for_username("alice") == POSTMERKOS_ROLE_ADMIN);
  assert(role_for_username("bob") == POSTMERKOS_ROLE_OPERATOR);
  assert(role_for_username("carol") == POSTMERKOS_ROLE_VIEWER);
  assert(role_for_username("nobody") == POSTMERKOS_ROLE_NONE);
  char username[64];
  assert(account_username_for_uid(1002, username, sizeof(username)) == 0);
  assert(!strcmp(username, "bob"));
  for (int i = 0; i < 1000; i++) {
    assert(role_for_username("alice") == POSTMERKOS_ROLE_ADMIN);
    assert(role_for_username("bob") == POSTMERKOS_ROLE_OPERATOR);
    assert(role_for_username("carol") == POSTMERKOS_ROLE_VIEWER);
    assert(role_for_uid(0) == POSTMERKOS_ROLE_ADMIN);
  }
  puts("role resolution repeatability tests passed");
  return 0;
}
