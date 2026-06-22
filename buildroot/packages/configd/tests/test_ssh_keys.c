#include "ssh_keys.h"
#include "config_file.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool dry_run = false;
const char *config_file = "/tmp/configd-ssh-test.json";

int main(void) {
  char err[256];

  /* valid keys of several types */
  assert(ssh_key_validate(
    "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAILhRkE7gtT0unKsa laptop", err,
    sizeof(err)) == 0);
  assert(ssh_key_validate(
    "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAB key@rsa", err, sizeof(err)) == 0);
  assert(ssh_key_validate(
    "ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTY=", err,
    sizeof(err)) == 0);

  /* rejects: empty, multiline, unknown/option-prefixed type, bad base64 */
  assert(ssh_key_validate("", err, sizeof(err)) != 0);
  assert(ssh_key_validate("ssh-ed25519 AAAA\nfoo bar", err, sizeof(err)) != 0);
  assert(ssh_key_validate("command=\"x\" ssh-ed25519 AAAAC3Nz", err,
                          sizeof(err)) != 0);
  assert(ssh_key_validate("ssh-dss AAAAB3Nz", err, sizeof(err)) != 0);
  assert(ssh_key_validate("ssh-ed25519 not*base64!", err, sizeof(err)) != 0);

  /* rejects: control character in comment, and over-length input */
  assert(ssh_key_validate("ssh-ed25519 AAAAB3Nz comment\x01here", err,
                          sizeof(err)) != 0);
  char long_key[17000];
  memset(long_key, 'A', sizeof(long_key) - 1);
  long_key[sizeof(long_key) - 1] = '\0';
  assert(ssh_key_validate(long_key, err, sizeof(err)) != 0);

  /* identity strips the comment but keeps type+blob */
  char id1[512], id2[512];
  assert(ssh_key_identity("ssh-ed25519 AAAAC3NzaC1lZDI1 alice", id1,
                          sizeof(id1)) == 0);
  assert(ssh_key_identity("ssh-ed25519 AAAAC3NzaC1lZDI1 bob", id2,
                          sizeof(id2)) == 0);
  assert(strcmp(id1, id2) == 0);
  assert(strcmp(id1, "ssh-ed25519 AAAAC3NzaC1lZDI1") == 0);

  /* identity reports failure when the output buffer is too small */
  char tiny[10];
  assert(ssh_key_identity("ssh-ed25519 AAAAC3NzaC1lZDI1", tiny,
                          sizeof(tiny)) != 0);

  puts("ssh_keys validation tests passed");
  return 0;
}
