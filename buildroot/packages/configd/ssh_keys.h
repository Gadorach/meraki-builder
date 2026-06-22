#ifndef CONFIGD_SSH_KEYS_H
#define CONFIGD_SSH_KEYS_H

#include <json-c/json.h>
#include <stddef.h>

/* Validate a single SSH public-key line: "<type> <base64> [comment]".
 * Returns 0 if acceptable; negative errno with a message in error otherwise. */
int ssh_key_validate(const char *line, char *error, size_t error_size);

/* Write the canonical identity ("<type> <base64>", comment stripped) of line
 * into out. Returns 0 on success, -EINVAL if line is not a valid key shape. */
int ssh_key_identity(const char *line, char *out, size_t out_size);

/* List configured root authorized keys as a JSON array of {label, key}. */
struct json_object *ssh_keys_list(void);

/* Add a validated key (deduped by identity); persists switch.json + renders. */
int ssh_keys_add(const char *label, const char *key,
                 char *error, size_t error_size);

/* Remove the entry whose stored key matches exactly; persists + renders. */
int ssh_keys_remove(const char *key, char *error, size_t error_size);

/* Render /root/.ssh/authorized_keys from switch.json ssh.authorized_keys.
 * Overrides for tests: $CONFIGD_SSH_DIR (dir), $CONFIGD_AUTHORIZED_KEYS (file). */
int ssh_keys_render(char *error, size_t error_size);

#endif
