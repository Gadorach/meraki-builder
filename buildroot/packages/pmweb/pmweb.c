// pmweb — single-origin TLS front for postmerkOS.
//
// Terminates TLS on :443, serves the static UI from a docroot, and reverse-
// proxies the management WebSocket (path /ws) to configd's plain ws on
// 127.0.0.1:4001. :80 redirects to https. If no *valid* cert/key pair is present
// it degrades to plain HTTP on :80 that STILL serves the UI and proxies /ws, so
// management stays reachable even without a cert. TLS uses mbedTLS (already in the
// image); the WebSocket is bridged at the message level so binary frames (firmware
// upload) and the "configd-ws" subprotocol are preserved.
//
// Dual-licensed via mongoose (GPL-2.0). Config via env:
//   PMWEB_CERT (/config/certs/web.crt)  PMWEB_KEY (/config/certs/web.key)
//   PMWEB_ROOT (/www)  PMWEB_BACKEND (ws://127.0.0.1:4001)
//   PMWEB_HTTPS_PORT (443)  PMWEB_HTTP_PORT (80)
#include "mongoose.h"

#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *s_cert, *s_key, *s_root, *s_backend;
static int s_tls = 0;  // main request listener terminates TLS
static volatile sig_atomic_t s_running = 1;
static void on_signal(int sig) { (void) sig; s_running = 0; }

#define PMWEB_WS_URI "/ws"
#define PMWEB_SUBPROTO_TOKEN "configd-ws"
#define PMWEB_SUBPROTO "Sec-WebSocket-Protocol: " PMWEB_SUBPROTO_TOKEN "\r\n"
#define PMWEB_PENDING_MAX (64 * 1024)    // per-pair pre-handshake queue cap
#define PMWEB_SEND_MAX (512 * 1024)      // per-connection outbound backlog cap
// Max concurrent proxied WS sessions. Each pair opens exactly one backend ws to
// configd, so this consumes one configd client slot per pair; kept in step with
// configd's MAX_CLIENTS (websocket.h, 16) which is the ultimate backstop -- if
// pmweb ever exceeded it, configd would reject the extra backend connect and the
// pair is torn down here.
#define PMWEB_MAX_PAIRS 16

static int s_pairs = 0;  // active front<->back pairs (remote-DoS guard)

// A front (browser) <-> back (configd) WebSocket pair. Front messages that arrive
// before the backend handshake completes are queued and replayed on open.
struct pair {
  struct mg_connection *front;
  struct mg_connection *back;
  int ready;              // backend WS handshake complete
  struct mg_iobuf queue;  // records: [op:1][len:4 LE][payload]
};

static void pair_free(struct pair *p) {
  mg_iobuf_free(&p->queue);
  free(p);
  if (s_pairs > 0) s_pairs--;
}

// Forward a WS frame to dst. If dst's outbound backlog is already large (the peer
// drains slower than the source floods), drain the source rather than growing the
// send buffer without bound -- otherwise a remote client can OOM this root process.
static void forward(struct mg_connection *dst, struct mg_connection *src,
                    const void *data, size_t len, int op) {
  if (!dst) return;
  if (dst->send.len > PMWEB_SEND_MAX) {
    if (src) src->is_draining = 1;
    return;
  }
  mg_ws_send(dst, data, len, op);
}

// Detach one side; free the pair once both sides are gone.
static void pair_detach(struct pair *p, int is_front) {
  if (!p) return;
  if (is_front) p->front = NULL; else p->back = NULL;
  if (!p->front && !p->back) pair_free(p);
}

static int queue_msg(struct pair *p, int op, const void *data, size_t len) {
  if (p->queue.len + 5 + len > PMWEB_PENDING_MAX) return -1;
  unsigned char hdr[5];
  hdr[0] = (unsigned char) op;
  hdr[1] = (unsigned char) (len & 0xff);
  hdr[2] = (unsigned char) ((len >> 8) & 0xff);
  hdr[3] = (unsigned char) ((len >> 16) & 0xff);
  hdr[4] = (unsigned char) ((len >> 24) & 0xff);
  mg_iobuf_add(&p->queue, p->queue.len, hdr, sizeof(hdr));
  mg_iobuf_add(&p->queue, p->queue.len, data, len);
  return 0;
}

static void queue_flush(struct pair *p) {
  size_t off = 0;
  while (off + 5 <= p->queue.len && p->back) {
    unsigned char *r = p->queue.buf + off;
    int op = r[0];
    size_t len = (size_t) r[1] | ((size_t) r[2] << 8) | ((size_t) r[3] << 16) |
                 ((size_t) r[4] << 24);
    if (off + 5 + len > p->queue.len) break;
    forward(p->back, p->front, r + 5, len, op);
    off += 5 + len;
  }
  mg_iobuf_free(&p->queue);
}

// configd-side WebSocket client.
static void back_fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  struct pair *p = (struct pair *) fn_data;
  if (!p) return;
  if (ev == MG_EV_WS_OPEN) {
    p->ready = 1;
    queue_flush(p);
  } else if (ev == MG_EV_WS_MSG) {
    struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
    forward(p->front, c, wm->data.ptr, wm->data.len, wm->flags & 15);
  } else if (ev == MG_EV_ERROR || ev == MG_EV_CLOSE) {
    if (p->front) p->front->is_draining = 1;
    c->fn_data = NULL;
    pair_detach(p, 0);
  }
}

// True iff the request's Sec-WebSocket-Protocol list contains the exact
// "configd-ws" token. configd speaks only that subprotocol, so pmweb enforces the
// same contract instead of upgrading any /ws request.
static int wants_configd_ws(struct mg_http_message *hm) {
  struct mg_str *hdr = mg_http_get_header(hm, "Sec-WebSocket-Protocol");
  if (!hdr || hdr->len == 0) return 0;
  const char *tok = PMWEB_SUBPROTO_TOKEN;
  size_t tlen = strlen(tok);
  for (size_t i = 0; i + tlen <= hdr->len; i++) {
    if (mg_ncasecmp(&hdr->ptr[i], tok, tlen) != 0) continue;
    char before = i > 0 ? hdr->ptr[i - 1] : ',';
    char after = (i + tlen < hdr->len) ? hdr->ptr[i + tlen] : ',';
    int lb = before == ',' || before == ' ' || before == '\t';
    int rb = after == ',' || after == ' ' || after == '\t';
    if (lb && rb) return 1;
  }
  return 0;
}

// Unified request listener (used for both the TLS :443 front and the cert-less
// HTTP :80 fallback): static files, /ws upgrade, then front WS bridging. TLS is
// initialised on accept only when serving HTTPS.
static void serve_fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  (void) fn_data;
  if (ev == MG_EV_ACCEPT) {
    if (s_tls) {
      struct mg_tls_opts opts = {0};
      opts.cert = s_cert;
      opts.certkey = s_key;
      mg_tls_init(c, &opts);
    }
  } else if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    if (mg_http_match_uri(hm, PMWEB_WS_URI)) {
      if (!wants_configd_ws(hm)) {  // enforce configd's only subprotocol
        mg_http_reply(c, 400, "", "expected the configd-ws subprotocol\n");
        c->is_draining = 1;
        return;
      }
      if (s_pairs >= PMWEB_MAX_PAIRS) {  // cap concurrent sessions (DoS guard)
        mg_http_reply(c, 503, "", "too many sessions\n");
        c->is_draining = 1;
        return;
      }
      /* mg_ws_upgrade auto-echoes the request's Sec-WebSocket-Protocol into the
         101; passing our own would duplicate the header and the browser rejects
         it (1006). Only the backend client-connect needs the explicit subproto. */
      mg_ws_upgrade(c, hm, NULL);
      if (!c->is_websocket) return;  // upgrade rejected (e.g. missing WS key)
      struct pair *p = (struct pair *) calloc(1, sizeof(*p));
      if (!p) { c->is_draining = 1; return; }
      s_pairs++;
      p->front = c;
      c->fn_data = p;
      p->back = mg_ws_connect(c->mgr, s_backend, back_fn, p, "%s", PMWEB_SUBPROTO);
      if (!p->back) { c->fn_data = NULL; pair_free(p); c->is_draining = 1; }
    } else if (hm->uri.len > 1 && hm->uri.ptr[hm->uri.len - 1] == '/') {
      mg_http_reply(c, 404, "", "not found\n");  // no directory listings
    } else {
      struct mg_http_serve_opts opts = {0};
      opts.root_dir = s_root;
      mg_http_serve_dir(c, hm, &opts);
    }
  } else if (ev == MG_EV_WS_MSG) {
    struct pair *p = (struct pair *) c->fn_data;
    struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
    if (!p) return;
    if (p->ready) {
      forward(p->back, c, wm->data.ptr, wm->data.len, wm->flags & 15);
    } else if (queue_msg(p, wm->flags & 15, wm->data.ptr, wm->data.len) != 0) {
      c->is_draining = 1;  // pending overflow before backend ready
    }
  } else if (ev == MG_EV_CLOSE) {
    struct pair *p = (struct pair *) c->fn_data;
    if (p) {
      if (p->back) p->back->is_draining = 1;
      c->fn_data = NULL;
      pair_detach(p, 1);
    }
  }
}

// Plain :80 listener: 301 redirect to https on the same host. The authority is
// the client's Host header -- standard scheme-upgrade behaviour (like nginx's
// `return 301 https://$host`), and the ONLY reliable source on this platform: the
// management IP is a Click/datapath address, not a Linux interface, so both
// getsockname() and mongoose's c->loc yield an internal/wildcard address, not the
// address the client actually used. Host is charset-validated (rejects CR/LF and
// other header/URL injection) and its :port is stripped.
static void redirect_fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  (void) fn_data;
  if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    struct mg_str *host = mg_http_get_header(hm, "Host");
    struct mg_str h = host ? *host : mg_str("");
    if (h.len == 0) { mg_http_reply(c, 400, "", "missing Host header\n"); return; }
    for (size_t i = 0; i < h.len; i++) {  // hostname/IP charset only
      char ch = h.ptr[i];
      if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == ':' ||
            ch == '[' || ch == ']')) {
        mg_http_reply(c, 400, "", "bad Host header\n");
        return;
      }
    }
    /* Strip any :port (redirect to default 443). For a bracketed IPv6 literal
       keep everything up to and incl. ']'; otherwise cut at the first ':'. */
    size_t hlen = h.len;
    if (h.len > 0 && h.ptr[0] == '[') {
      for (size_t i = 0; i < h.len; i++) if (h.ptr[i] == ']') { hlen = i + 1; break; }
    } else {
      for (size_t i = 0; i < h.len; i++) if (h.ptr[i] == ':') { hlen = i; break; }
    }
    char loc[600];
    snprintf(loc, sizeof(loc), "Location: https://%.*s%.*s\r\n", (int) hlen,
             h.ptr, (int) hm->uri.len, hm->uri.ptr);
    mg_http_reply(c, 301, loc, "");
  }
}

static const char *env_or(const char *name, const char *fallback) {
  const char *v = getenv(name);
  return v && *v ? v : fallback;
}

static int file_exists(const char *path) {
  FILE *f = path ? fopen(path, "r") : NULL;
  if (f) { fclose(f); return 1; }
  return 0;
}

// Validate that cert_path/key_path parse and form a matching pair (mbedTLS). A
// present-but-broken pair must NOT put us into HTTPS mode -- otherwise :80 would
// 301 into a TLS endpoint whose handshake always fails, locking out management.
static int cert_pair_valid(const char *cert_path, const char *key_path) {
  if (!file_exists(cert_path) || !file_exists(key_path)) return 0;
  mbedtls_x509_crt crt;
  mbedtls_pk_context pk;
  mbedtls_x509_crt_init(&crt);
  mbedtls_pk_init(&pk);
  int ok = 0;
  if (mbedtls_x509_crt_parse_file(&crt, cert_path) == 0 &&
      mbedtls_pk_parse_keyfile(&pk, key_path, NULL) == 0 &&
      mbedtls_pk_check_pair(&crt.pk, &pk) == 0)
    ok = 1;
  mbedtls_pk_free(&pk);
  mbedtls_x509_crt_free(&crt);
  return ok;
}

int main(void) {
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);

  s_cert = env_or("PMWEB_CERT", "/config/certs/web.crt");
  s_key = env_or("PMWEB_KEY", "/config/certs/web.key");
  s_root = env_or("PMWEB_ROOT", "/www");
  s_backend = env_or("PMWEB_BACKEND", "ws://127.0.0.1:4001");
  const char *https_port = env_or("PMWEB_HTTPS_PORT", "443");
  const char *http_port = env_or("PMWEB_HTTP_PORT", "80");

  struct mg_mgr mgr;
  mg_mgr_init(&mgr);

  char url[64];
  if (cert_pair_valid(s_cert, s_key)) {
    s_tls = 1;
    // Primary serving listener (HTTPS). If it cannot bind, exit non-zero rather
    // than run uselessly -- otherwise the :80 redirect would still send clients
    // to a dead HTTPS endpoint.
    mg_snprintf(url, sizeof(url), "https://0.0.0.0:%s", https_port);
    if (!mg_http_listen(&mgr, url, serve_fn, NULL)) {
      MG_ERROR(("FATAL: cannot bind %s", url));
      mg_mgr_free(&mgr);
      return 1;
    }
    // Secondary :80 -> :443 redirect. Non-fatal: HTTPS is already up; log loudly.
    mg_snprintf(url, sizeof(url), "http://0.0.0.0:%s", http_port);
    if (!mg_http_listen(&mgr, url, redirect_fn, NULL))
      MG_ERROR(("DEGRADED: cannot bind %s for http->https redirect; https still up", url));
    MG_INFO(("pmweb: https :%s (cert=%s), redirect :%s", https_port, s_cert, http_port));
  } else {
    // No valid cert -> serve HTTP-only on :80, but still proxy /ws so the UI is
    // fully usable (not just static) until a cert is installed. This is the sole
    // serving listener here, so a bind failure is fatal.
    s_tls = 0;
    mg_snprintf(url, sizeof(url), "http://0.0.0.0:%s", http_port);
    if (!mg_http_listen(&mgr, url, serve_fn, NULL)) {
      MG_ERROR(("FATAL: cannot bind %s", url));
      mg_mgr_free(&mgr);
      return 1;
    }
    if (file_exists(s_cert) || file_exists(s_key))
      MG_ERROR(("pmweb: cert/key at %s is missing or invalid; serving HTTP (incl. /ws) on :%s",
                s_cert, http_port));
    else
      MG_INFO(("pmweb: no cert at %s; serving HTTP (incl. /ws) on :%s", s_cert, http_port));
  }

  while (s_running) mg_mgr_poll(&mgr, 1000);
  mg_mgr_free(&mgr);
  return 0;
}
