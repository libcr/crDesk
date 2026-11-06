# crDeskServer WebSocket protocol v1

Endpoint: `wss://YOUR_HOST/v1/ws`, proxied to `ws://127.0.0.1:9080/v1/ws`.
Use RFC 6455 version 13, masked client frames, UTF-8 JSON text messages, no
compression, no browser `Origin`. Every message is an object with a `type` string.
Binary messages are unsupported. Additional fields are ignored for forward
compatibility; unsupported message types receive an error. Invalid JSON closes
the connection. Do not send media or file contents through this protocol.

## Authentication

The server immediately sends:

```json
{"type":"challenge","version":1,"nonce":"64 lowercase hex characters","features":["fast_reconnect_v1"]}
```

The client computes a proof with its provisioned 32-byte secret:

```python
import hashlib
import hmac

key = bytes.fromhex(device_secret)
data = ("crdesk-server-v1\n" + nonce + "\n" + device_id).encode("utf-8")
proof = hmac.new(key, data, hashlib.sha256).hexdigest()
```

Send within 10 seconds of opening TCP:

```json
{"type":"authenticate","version":1,"device_id":"laptop","proof":"64 hex characters"}
```

Only authentication is allowed before `ready`. A proof is bound to the fresh
connection nonce and the exact device ID. A failed proof sends the generic
`authentication_failed` error on a best-effort basis and closes the connection,
without revealing whether the ID exists. An already-online authenticated device
receives `already_online` on a best-effort basis, then the new socket is closed.
This is a transient ownership conflict, not an invalid credential: clients retry
with bounded exponential backoff and jitter instead of requiring sign-in again.

### Fast reconnect

When the challenge advertises `fast_reconnect_v1`, the native broker adds three
fields to `authenticate`, in addition to the ordinary proof above:

- `client_instance`: a fresh random 32-byte value encoded as 64 lowercase hex
  characters, generated once per broker process and never persisted or logged.
- `connection_generation`: a positive uint64 encoded as a canonical decimal
  string, incremented for every transport-opening attempt in that process.
- `reconnect_proof`: HMAC-SHA256 with the device secret over the UTF-8 string
  `crdesk-reconnect-v1\n<nonce>\n<device_id>\n<client_instance>\n<connection_generation>`.

All three fields must be present and valid together. Both proofs use the fresh
server nonce. The server accepts a replacement only after verifying both proofs,
when the old socket belongs to the same instance and the generation is strictly
newer. A delayed older attempt, a different process using copied credentials, or
a client without this extension cannot evict an active connection. Different
processes may still need to wait for an orphaned socket's idle timeout.

The server installs the new online owner before closing the old socket. Old
socket cleanup ends its signaling sessions, but cannot remove the new presence
or mark the replacement offline. The new socket starts with sharing disabled;
the client republishes its actual sharing state. Session IDs are not resumed.
The process-local instance and generation survive failed handshakes, lost ready
replies and transport resets; no server-issued recovery token needs persisting.
Clients only send this extension when advertised, so old servers continue to use
the original authentication exchange. New servers accept original clients too.

```json
{"type":"ready","version":1,"device_id":"laptop","heartbeat_seconds":15,"idle_timeout_seconds":45,"features":["connection_check","account_tools","fast_reconnect_v1"]}
```

WSS certificate validation authenticates the service; never disable it. The
nonce/proof exchange authenticates the device to this service. It does not
replace crDesk's end-to-end pairing/key exchange.

## Heartbeats and discovery

Send `{"type":"ping"}` every 15 seconds, including when no tab is active.
Expect `{"type":"pong"}`. Any valid application message refreshes idle time;
WebSocket ping/pong control frames alone do not. On failure, reconnect with
exponential backoff and jitter, authenticate again, request a fresh device list,
and republish sharing state. Session IDs are never resumed across reconnects.
The native client uses a 10-second application-response deadline after a ping;
any received valid JSON message clears that deadline. Network-available events
are debounced for 500 ms: a pending reconnect retries early with reset backoff,
while an established connection is probed without immediately closing it.
An outstanding probe deadline is not extended by repeated network events.
Explicit authentication errors and certificate failures remain terminal until explicit action;
a network event does not restart a disabled or terminally failed connection.
Closing the socket between challenge and ready without an error is treated as
a network failure and retried; it does not prove that credentials were rejected.

Request `{"type":"list_devices"}`:

```json
{"type":"devices","devices":[{"id":"office","name":"Office desktop","online":true,"sharing":false}]}
```

The list contains only the requesting device's configured `allowed_hosts`.
Subsequent presence changes arrive automatically on authorized viewers:

```json
{"type":"device_changed","device":{"id":"office","name":"Office desktop","online":true,"sharing":true}}
```

The host advertises willingness to receive requests:

```json
{"type":"publish","sharing":true}
```

Response: `{"type":"published","sharing":true}`. Initial state is false.
Publishing false closes all incoming pending/accepted sessions with
`sharing_stopped`; outgoing viewer sessions remain independent. Presence events
and replies can interleave; dispatch by type and session/request ID.

## Infrastructure diagnostics

Servers advertising `connection_check` in `ready.features` accept the following
command after device authentication, without opening a desktop session:

```json
{"type":"diagnostic_ice","request_id":"check_1"}
```

The reply is `{"type":"diagnostic_ice","request_id":"check_1","servers":[...]}`.
`servers` has the same shape and short-lived credentials as the session-scoped
`ice_servers` response. It never contains the TURN shared secret. The request ID
uses the same validation as connection requests. Requests less than 30 seconds
apart on the same connection receive `rate_limited` with the request ID.
Clients without this feature continue to use session-scoped credentials normally.
A client must not assume an infrastructure test proves connectivity to a peer.

## Request and acceptance

Viewer to server:

```json
{"type":"connect","request_id":"request_1","target":"office"}
```

`request_id` is a client-generated correlation ID of 1–64 ASCII letters, digits,
`_` or `-`; it must be unique among that viewer's live requests/sessions. It is
not a credential. The server checks the directed allowlist and host availability,
assigns a random session ID, and sends the viewer:

```json
{"type":"pending","request_id":"request_1","session_id":"SERVER_ASSIGNED_ID","timeout_seconds":30}
```

The host receives:

```json
{"type":"incoming","session_id":"SERVER_ASSIGNED_ID","viewer":"laptop","name":"Travel laptop","timeout_seconds":30}
```

Only the named host may send `{"type":"accept","session_id":"..."}` or
`{"type":"reject","session_id":"..."}`. Acceptance sends
`{"type":"accepted","session_id":"..."}` to both participants. This permits
signaling exchange; it does **not** grant desktop control. The host's unattended
policy or local confirmation controls whether to accept, and the normal crDesk
peer authentication still runs before media/control is permitted.

## Signaling

### Cross-account access tokens (`access_tokens_v1`)

Account-enabled servers advertise `access_tokens_v1` in `ready.features`.
An authenticated, currently sharing host may request a temporary routing grant:

```json
{"type":"access_token_create","request_id":"token_REQUEST","minutes":15}
```

Allowed lifetimes are 15, 60, 720 or 1440 minutes. The server replies with
`access_token_created`, the same `request_id`, a 32-hex-character `id`, a
64-hex-character `secret`, and an `issued_ms` Unix timestamp string. The host
uses this ID and issue time when minting its ordinary access token, adding a
`cloud` object containing `url`, `device` and `secret`. Its independent peer
authentication key is generated locally and is **never sent to the server**.
The server stores only a hash of the cloud secret, bound to the issuing device,
its current enrollment credential and a monotonic expiration deadline.

A viewer on any account registered with this server sends:

```json
{"type":"connect","request_id":"request_2","target":"HOST_DEVICE_ID","token_id":"TOKEN_ID","token_secret":"CLOUD_SECRET"}
```

Supplying token fields requires a valid grant even if the accounts happen to
match. The grant relaxes authorization for this connection only; `list_devices`,
presence and `avatar_get` retain their account boundaries. The viewer's first
relayed signal must be the existing peer `hello` with `identity=token:TOKEN_ID`;
the server checks that the grant has not expired or been revoked before forwarding
it. Other authentication methods cannot be substituted in that session. The
host verifies the proof, expiry and connection limit through its existing
`AccessTokenStore`. This single counter is authoritative for both routes;
the server does not maintain a separate count that could double-charge or
miss direct connections. Tokens do not bypass local approval or trusted-only
host policy. Routing authorization alone does not authenticate a peer.

The owner can send `{"type":"access_token_revoke","token_id":"TOKEN_ID"}`.
Generation rollback uses the same command. Publishing `sharing=false` removes
all grants for that host and ends its cloud sessions. Grants are memory-only,
expire automatically, and are bounded to 64 per device / 16384 per server.
Creation and connection attempts share the existing per-connection request
budget. The clients time out grant requests and revoke late successful replies.
Do not log either token secret or complete encoded access tokens.

### Peer messages

After acceptance, either participant can send:

```json
{"type":"signal","session_id":"...","payload":"OPAQUE_CRDESK_PROTOCOL_MESSAGE"}
```

The other participant receives:

```json
{"type":"signal","session_id":"...","from":"laptop","payload":"OPAQUE_CRDESK_PROTOCOL_MESSAGE"}
```

The server supplies `from` from the authenticated connection. It ignores any
sender-supplied destination/from field and forwards only within the approved
session. `payload` is a nonempty string, at most 48 KiB in UTF-8; the entire JSON
envelope must fit 64 KiB after escaping. A relay envelope that exceeds the limit
is rejected with `invalid_payload` to the sender.

Client integration should carry the existing crDesk peer challenge/proof
messages first, then its authenticated SDP/ICE envelopes. Preserve sequence/MAC
verification and bind the transport to the expected peer/session; server login
must never bypass existing trusted-device and access-token authorization. Queue
early ICE until the peer connection's remote description is ready. The server
does not interpret, validate, or rewrite SDP or ICE candidates.

## ICE configuration

Either participant in an accepted session may send:

```json
{"type":"ice_servers","session_id":"..."}
```

Example response:

```json
{
  "type":"ice_servers",
  "session_id":"...",
  "servers":[
    {"urls":["stun:turn.example.com:3478"]},
    {
      "urls":["turn:turn.example.com:3478?transport=udp"],
      "username":"UNIX_EXPIRY_SECONDS:laptop",
      "credential":"BASE64_HMAC_SHA1",
      "ttl_seconds":600
    }
  ]
}
```

The credential is Base64(HMAC-SHA1(ASCII coturn shared secret, username)). Do not
hex-decode the coturn secret. The server never returns that shared secret.
`servers` can be empty. Fetch credentials before constructing the peer connection;
refresh them before expiry and update ICE configuration as needed for long
sessions/ICE restarts. Expiry is not a command to terminate media. Only authenticated,
accepted participants can obtain credentials; they remain bearer credentials
subject to coturn quotas and lifetime rules.

## Closure and errors

Either participant can send `{"type":"close","session_id":"..."}`. Both
receive `{"type":"closed","session_id":"...","reason":"peer_closed"}`.
Other server-owned reasons are `rejected`, `timeout`, `peer_offline`, and
`sharing_stopped`. Participants must stop the associated peer transport and
release resources; loss of the server connection invalidates all its sessions.

Request errors have the shape:

```json
{"type":"error","code":"unavailable","request_id":"request_1"}
```

`request_id` is included for a structurally valid `connect` request. Other errors
do not include it; serialize state-changing operations per session and do not
rely on errors as payload acknowledgments.

| Code | Meaning |
| --- | --- |
| `authentication_failed` | Device authentication or the reconnect proof was rejected. |
| `already_online` | Another connection owns this device ID; retry with backoff. |
| `invalid_request` | Required fields/types or request ID are invalid. |
| `not_allowed` | Target is outside the viewer's allowlist. |
| `unavailable` | Allowed target is offline or not sharing. |
| `busy` | Host already has an incoming pending/accepted session. |
| `duplicate_request` | Viewer reused an outstanding request ID. |
| `session_limit` | Viewer already has four outgoing sessions. |
| `rate_limited` | Connection request budget exhausted. |
| `unknown_session` | Session is absent or caller is not a participant. |
| `invalid_state` | Wrong role, already accepted, or signaling before acceptance. |
| `invalid_payload` | Payload missing, empty, or exceeds its byte/envelope limit. |
| `unknown_message` | Unsupported application message type. |

Malformed/authentication/size/rate violations and write queue overflow can close
the socket directly; a final error message is not guaranteed. On server restart
or proxy/network failure, the TCP/WS close itself is the notification. Clients
must not depend on receiving `closed` during a transport failure.

## Optional account mode

With an `accounts` configuration block, the native WebSocket challenge/proof
protocol is unchanged, but credentials come from browser-approved device
enrollment. Discovery and connection requests are authorized by shared account
ID. Static device credentials are not used in this mode.

HTTPS account endpoints are documented in [ACCOUNTS.md](ACCOUNTS.md). The native
client POSTs `{name, challenge, identity_key}` to `/auth/device/start`, which
returns `{flow, login_url, native_confirmation:true, expires_in:600}`. Before
opening `login_url`, the client proves its installation key through
`/auth/device/prove` as described in the account guide. `challenge` is unpadded
base64url of SHA-256 of a 32-byte random, base64url-encoded verifier. The client
polls `/auth/device/poll` with `{flow, verifier}` every three seconds. The
browser separately signs in and clicks **Register this device**. For a new
device, polling returns `{status:"confirmation", account, email, confirmation}`;
it cannot redeem credentials. After the user checks the displayed account and
server and clicks **Confirm sign-in** in crDesk, the native client POSTs
`{flow, verifier, account, confirmation}` to `/auth/device/confirm`. Its one-shot
successful response is `{status:"complete", restoring:false, url, device_id,
secret, account, email}`. Recognized installations signing in to the same
account can instead receive `complete` with `restoring:true` from polling.
Credentials and the native confirmation challenge must never be sent to the
browser or logged. Login requests expire in ten minutes. All JSON endpoints
require `application/json`. Deploy matching native clients, server and web assets;
there is no fallback to the former user-entered registration code.

Browser POSTs include the context `flow` and `csrf`. `/auth/browser/start`
accepts `{flow?, device?, management?}`; `management:true` permits recreation
of a missing management context only, never a native enrollment. The browser
uses this hint only on management pages. A valid persistent account cookie
restores the signed-in account into the context. A device ID remains a display
hint, not authentication. `browser/logout` deletes that persistent session and
all pending contexts bound to it, and clears both cookies.

Browser account sessions use a fixed 30-day Secure/HttpOnly/SameSite=Lax cookie;
the database stores only an origin-bound token hash and expiry. They survive
server restarts. Ten-minute enrollment requests and two-minute WebAuthn
challenges do not. Browser POSTs use a separate short-lived context cookie (or
its bound persistent session for an explicitly named authenticated context), exact Origin
checking and a CSRF token. Google callbacks additionally check state and nonce.
Passkey operations validate WebAuthn origin, RP hash, challenge, UP/UV flags,
public-key signature and signature-counter policy. See the account deployment
guide for the required domain, OAuth client type and supported algorithms.

### Passkey account registration

`/auth/passkey/options` accepts `{csrf, operation, name?}`. Operations are
`get` (signed-out login), `create` (add a credential to a signed-in account), and
`register` (create a new signed-out account). Registration requires a nonempty
UTF-8 nickname of at most 64 bytes with no control characters. Options requests
are rate-limited. The generated account ID and nickname remain in the flow,
not in the database, until registration verification succeeds.

`/auth/passkey/finish` must carry the same operation plus the existing WebAuthn
response fields. `register` and `create` both require `webauthn.create`; the
flow's single-use challenge distinguishes their purposes. Registration inserts
the account and first passkey in one transaction, then authenticates the browser.
It neither approves a native device nor returns device credentials. Duplicate
credential IDs are rejected without leaving a new account.

`/auth/browser/start` also returns `account`. Passkey-only accounts have an empty
`email`; clients must display the account ID when email is empty and continue
binding confirmation to `account`. Nicknames are display metadata only and are
never used to match or merge accounts. No email verification is implied.

## Account tools extension

The authenticated browser endpoint `/auth/account/metadata` accepts
`{csrf, id, name, group, tags, note, order}` for an account-owned device. The
name is required and follows the existing device-name validation (non-empty,
valid UTF-8, at most 64 bytes, no control characters). All fields are validated
before a single device-record write, so rename and metadata updates succeed
or fail together. Deploy the matching server binary and web assets together.
The separate `/auth/account/rename` API remains available for passkey names
and existing API callers; the device page exposes only **Edit details**.

Servers advertising `account_tools` in `ready.features` accept the following
optional messages. Clients must feature-detect before sending them. Device
objects additionally contain `group` (string), `tags` (string array), `note`
(string) and `order` (integer). In account mode, discovery lists other devices
of the same account, including unavailable devices; these fields are not ACLs.

Only the viewer of an accepted signaling session may send:

```json
{"type":"session_report","session_id":"...","state":"connected","route":"direct"}
```

`state` is `connected`, `disconnected` or `failed`. Optional `route` is `direct`
or `relay`; changes are capped at eight per session. Disconnection after a
reported connection closes signaling with `peer_closed`. A `failed` report has
one `reason` from `authentication`, `pairing_authentication`, `capture`,
`encoding`, `protocol`, `sdp`, `webrtc_init`, `data_channel`, `signaling_timeout`,
`ice`, or `invalid_address_key`. At most one failure is saved per session; it
alone does not close signaling. All these observations are client-reported,
not independently verified by the signaling service.

An authenticated account device may send its completed self-check summary:

```json
{"type":"diagnostic_report","results":[{"stage":"turn_tls","result":"passed","probe":3}]}
```

There are at most 16 rows, with lowercase/underscore stage (32 bytes) and result
(40 bytes) identifiers. Allowed numeric fields are `probe`, `count`, `error_code`,
`net_error`, `http_status`, `relay_endpoints`, `side0_error`, `side1_error`. Other fields are discarded. Reports
are limited to one every 30 seconds per connection and replace the device's
previous report. These messages have no acknowledgment. Non-account servers do
not persist reports.

Client-to-server messages remain limited to 64 KiB. Server-to-client device
lists may use up to 256 KiB to accommodate metadata; signaling envelopes retain
their 64 KiB limit. The server's socket write-buffer limit is 512 KiB.

## Device avatars (`avatars_v1`)

Account-enabled servers advertise `avatars_v1` in `ready.features`. `ready`
contains the authenticated device's `avatar` (built-in index 0–8) and
`avatar_hash` (empty or a lowercase SHA-256). `devices` and `device_changed`
include the same fields for each visible device. Avatars belong to devices,
not to the Google/Passkey account.

- `avatar_update`: `{type, avatar, hash, data}`. `data` is base64 JPEG, at most
  24 KiB decoded, with a 256×256, 8-bit baseline/progressive JPEG header. The
  digest must match `hash`. Empty `hash` and `data` restore the built-in avatar.
  Updates apply only to the authenticated device, with a two-second minimum
  interval. `avatar_saved: {ok}` reports the outcome; rejected updates may be
  retried later. The device metadata and image are stored in one transaction.
- `avatar_get`: `{type, device, hash}`. The device must be visible under the
  same account/authorization rules as discovery and the requested hash must be
  current. `avatar_data: {device, hash, data}` returns the image; unavailable
  or unauthorized requests do not return image content. Clients time out the
  individual download after 15 seconds and continue with the next image.
- Discovery carries hashes only. Clients fetch missing images sequentially,
  validate the JPEG header and digest before storing, and decode only bounded
  256×256 images for display. The server stores one image per device; device
  deletion and restoring the built-in avatar remove that image.

Clients publish their saved avatar after authentication and on local changes.
They check for a pending update on the heartbeat (15 seconds), so an offline
change is sent after reconnection. Servers without the feature continue using
the built-in fallback. Deployment requires rebuilding both crDesk and
crDeskServer; no database schema migration is required.

## Temporary browser viewers

The native `/v1/ws` handshake above is unchanged. Browser viewers use
`/auth/remote/ws` on the configured account origin, with the account cookie
(scoped to `/auth/`) and exact Origin verification. Before opening the socket,
POST `/auth/remote/ticket` with the account context `flow` and `csrf` to obtain
`{ticket, expires_in:60}`. The ticket is one-use and bound to that account session.
After the server's `challenge`, reply with
`{type:"authenticate_browser", ticket:"..."}`. The `ready` message assigns a
temporary `web-...` identity. No native-device HMAC credential is issued.

The supported messages are `ping`, `list_devices`, `connect`, `signal`, `close`,
`ice_servers`, and `session_report`, with the existing message formats. Only
same-account hosts are allowed; browser `connect` cannot redeem access tokens.
These identities are not advertised as devices and cannot authenticate through
the native endpoint. Account session revocation/expiry closes their sockets and
associated sessions. See [BROWSER.md](BROWSER.md) for host authentication, file
protocol behavior, deployment and browser limitations.
