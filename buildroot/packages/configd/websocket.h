#ifndef CONFIGD_WEBSOCKET_H
#define CONFIGD_WEBSOCKET_H

#include <json-c/json.h>

#define MAX_CLIENTS 16
#define MAX_MSG_LEN 65536

struct lws_context;

/* configd serves plain ws on loopback; the pmweb TLS front proxies external wss. */
struct lws_context *ws_init(int port);
/* Where the cert lives (owned by configd's cert RPCs, served by pmweb). */
const char *ws_tls_cert_path(void);
const char *ws_tls_key_path(void);
void ws_schedule_timers(struct lws_context *context, int status_interval);
void mark_clients_pending(void);
int ws_service_once(struct lws_context *context, int timeout_ms);
void ws_shutdown(struct lws_context *context);
void ws_revoke_user_sessions(const char *username);
char *wrap_message(const char *type, struct json_object *data,
                   struct json_object *request_id);

#endif
