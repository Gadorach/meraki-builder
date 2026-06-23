#ifndef CONFIGD_SESSION_H
#define CONFIGD_SESSION_H

#include <stddef.h>

#define SESSION_TTL_DEFAULT 28800L   /* 8 hours  */
#define SESSION_TTL_REMEMBER 604800L /* 7 days   */

/* Mint a token for username valid for ttl_seconds from now (unix time).
 * Returns a pointer to the NUL-terminated 64-hex-char token (stored in the
 * table; copy it before the next session_* call), or NULL on RNG failure. */
const char *session_create(const char *username, long ttl_seconds, long now);

/* Look up a live, unexpired token (constant-time compare). On hit, copy the
 * username into out (size n) and return 0; otherwise return -1. */
int session_lookup(const char *token, long now, char *out_username, size_t n);

/* Revoke a single token (no-op if absent). */
void session_revoke_token(const char *token);

/* Revoke every token belonging to username (password change / account delete). */
void session_revoke_user(const char *username);

#endif
