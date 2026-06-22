# configd WebSocket and local protocol

## Transport

Web builds listen on TCP port 4001 and require the WebSocket subprotocol:

```text
configd-ws
```

The local console uses `/run/postmerkos/configd.sock`. Both transports resolve one role/capability session and dispatch to the same validation and operation handlers.

Local requests and replies are one JSON object followed by a newline. Both peers
read until the complete delimiter, enforce bounded message sizes and a five-second
I/O timeout, and suppress SIGPIPE so a disconnected peer cannot terminate either
configd or `postmerkosctl`.

Run `configd --features` to confirm which transports were compiled into an image.

## Envelope

Text messages are JSON objects with a string `type`, optional `id`, and optional `data`.

```json
{"id":"client-generated-id","type":"hello"}
```

Text requests are limited to 65,536 bytes and fragmented frames are reassembled before parsing. Binary WebSocket frames are accepted only during an authenticated, token-owned firmware upload.

## Pre-authentication allowlist

Only these request types are accepted before login:

- `hello`
- `ping`
- `auth`
- `logout`

A newly connected client receives `auth_required` and should validate the endpoint with `hello` before enabling the login action.

```json
{"id":"1","type":"hello"}
```

```json
{
  "id":"1",
  "type":"hello",
  "data":{
    "service":"configd",
    "protocol":2,
    "websocket_protocol":"configd-ws",
    "authentication_required":true,
    "firmware":"2026-06-18-01-30"
  }
}
```

Authentication uses a local Linux account with a postmerkOS role:

```json
{"id":"2","type":"auth","data":{"username":"root","password":"..."}}
```

A successful response includes username, UID, role, and capabilities. The complete user list is returned only to sessions with `users.manage`.

## Authenticated requests

### `get_status`

```json
{"id":"3","type":"get_status"}
```

Returns current best-effort device state. Missing sensors or Click handlers appear in `data.errors`; the rest of the status remains available.

### `get_config`

```json
{"id":"4","type":"get_config"}
```

Returns the complete persistent desired configuration.

### `config`

`data` is a partial configuration delta. It is deep-merged into the saved configuration, validated, atomically saved, and then applied.

```json
{"id":"5","type":"config","data":{"ports":{"4":{"poe":{"enabled":true,"mode":"af"}}}}}
```

A successful response is an `ack`. Configd then broadcasts the complete configuration and updated status to authenticated clients.

Every operation also checks the session capability. Hiding a control in the console or browser is not the security boundary.

## Responses

### Acknowledgement

```json
{"id":"5","type":"ack","data":{"message":"Configuration accepted","applied":2,"warnings":[]}}
```

Warnings mean the desired state was valid and saved but one or more runtime operations could not be verified or applied.

### Error

```json
{"id":"5","type":"error","data":{"status":400,"message":"Bad Request","detail":"ports.49.poe is not supported by port 49"}}
```

Authorization failures use status 401 or 403. Rejected configuration is not saved.

### Broadcasts

```json
{"type":"config","data":{}}
{"type":"status","data":{}}
```

Broadcasts are sent only after authentication and normally do not contain an `id`.

## CLI equivalents

```sh
configd --features
configd --get-status
configd --get-config
configd --apply-json '{"ports":{"1":{"enabled":false}}}'
configd --apply-file ./delta.json
configd --validate ./complete.json
postmerkosctl session --shell
```


## Runtime health probe

`postmerkosctl management-health` validates the local process and Unix socket, performs a role-aware local `session` request, opens a WebSocket on TCP 4001, requires the `configd-ws` subprotocol, and sends a protocol `hello`. This is the readiness contract used by `S15configd`.
