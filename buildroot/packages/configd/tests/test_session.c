#include "../session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  long now = 1000;
  char user[65];

  const char *t1 = session_create("root", 100, now);
  assert(t1 && strlen(t1) == 64);
  char tok1[65]; snprintf(tok1, sizeof(tok1), "%s", t1);
  assert(session_lookup(tok1, now + 50, user, sizeof(user)) == 0);
  assert(strcmp(user, "root") == 0);

  assert(session_lookup(tok1, now + 100, user, sizeof(user)) != 0);

  const char *t2 = session_create("alice", 100, now);
  char tok2[65]; snprintf(tok2, sizeof(tok2), "%s", t2);
  assert(session_lookup(tok2, now, user, sizeof(user)) == 0);
  session_revoke_token(tok2);
  assert(session_lookup(tok2, now, user, sizeof(user)) != 0);

  const char *t3 = session_create("bob", 100, now);
  char tok3[65]; snprintf(tok3, sizeof(tok3), "%s", t3);
  session_revoke_user("bob");
  assert(session_lookup(tok3, now, user, sizeof(user)) != 0);

  assert(session_lookup("short", now, user, sizeof(user)) != 0);
  const char *a = session_create("u", 100, now);
  char toka[65]; snprintf(toka, sizeof(toka), "%s", a);
  const char *b = session_create("u", 100, now);
  assert(strcmp(toka, b) != 0);

  puts("session tests passed");
  return 0;
}
