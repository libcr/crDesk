# crDeskServer

A single-process rendezvous service for a small, administrator-managed crDesk
deployment. It provides device authentication, permission-filtered discovery,
online/sharing status, connection requests, signaling relay, and optional
short-lived coturn credentials. It uses Chromium `base`, `net`, and `crypto`;
it does not load the desktop UI, capture devices, or WebRTC media engine.

Desktop client enrollment, WSS, discovery and ICE integration are available
through **Settings → Cloud connection**. See [CLIENT.md](CLIENT.md) for setup,
trust boundaries and the current requirement to keep the desktop UI running.
The client uses the existing [v1 protocol](PROTOCOL.md).

Optional [Google sign-in and passkeys](ACCOUNTS.md) enroll devices into accounts
with a private SQLite database. Devices in the same account can discover one
another; remote desktop access still requires the host's existing authorization.
See that guide for the browser login page, OAuth setup and deployment steps.

## Architecture

```text
crDesk A -- WSS/443 -- Nginx -- WS/127.0.0.1:9080 -- crDeskServer
crDesk B -- WSS/443 -- Nginx -----------+

crDesk A <========== WebRTC / ICE ==========> crDesk B
                   direct when possible
                       or via coturn
```

Nginx handles TLS and certificates. crDeskServer listens **only on IPv4 loopback**;
the proxy must run in the same network namespace. Do not expose port 9080 or
change it to a public address. Native clients must verify the WSS certificate
and send no `Origin` header; browser-origin upgrades are rejected. The server
does not use proxy headers as device identity.

Signaling is JSON over WebSocket/TCP. Media, input and file transfers belong on
WebRTC, not on this WebSocket. ICE determines direct connectivity; this service
cannot guarantee NAT traversal. STUN helps discover public candidates; TURN
provides a relay when direct paths fail. WireGuard is not required.

## Build

Use a Linux Chromium checkout including the accompanying `net/server` changes.
Those opt-in changes bound fragmented WebSocket messages and close slow clients
when their write queue overflows. They retain the default limits/overflow policy
for existing HttpServer users.

The Linux desktop build includes the server as a build dependency. With the
existing crDesk build configuration, the usual command generates both
`out/debian/crDesk` and `out/debian/crDeskServer`:

```sh
autoninja -C out/debian crDesk
```

To build only the server in that configuration:

```sh
autoninja -C out/debian crDeskServer
```

The output is `out/debian/crDeskServer`. The target has its own BUILD.gn and does
not depend on `//crDesk:crDesk`. For a separate server-only build configuration,
add `"//crDesk/server:crDeskServer"` to `root_extra_deps` in its `args.gn` and
regenerate that build directory with GN. Building the server does not install
or start it, or add it to the desktop installer.

Use a non-component release build for deployment. Install its matching
`icudtl.dat` beside `crDeskServer` for URL/HTTPS support.
A component build additionally needs its shared libraries and matching runtime
search path. Match the build's Linux runtime requirements to the server OS.

## Configuration and enrollment

Copy [server.example.json](deploy/server.example.json) to a private working
directory. The example deliberately contains invalid device-secret placeholders
so it cannot start with publicly known credentials. Generate a different secret
for each device, and a separate one for coturn:

```sh
out/debian/crDeskServer --generate-secret
```

Replace each device's `secret` with one generated 64-character hex string. Give
that device only its own ID, secret and the WSS URL, through a trusted channel.
Never distribute the server config or the coturn shared secret to clients. These
credentials are separate from crDesk pairing keys and access tokens.

`allowed_hosts` is directional: putting `office` in `laptop.allowed_hosts`
allows the laptop to discover and request a connection to the office device.
It does not grant the office device access to the laptop. The example explicitly
allows both directions. An empty list permits a host-only device. A target must
also be online, publish `sharing: true`, and accept the connection request.
The host must still perform crDesk's existing peer authentication and apply its
local remote-control, clipboard and file permissions.

Supported configuration fields:

| Field | Meaning |
| --- | --- |
| `version` | Required; `1`. |
| `listen_port` | Optional; loopback port, 1024–65535; default 9080. |
| `max_connections` | Optional; 2–1024 HTTP/WS connections; default 256. |
| `devices` | Required; 1–128 static devices, each with `id`, `name`, `secret`, `allowed_hosts`; use an empty array in account mode. |
| `accounts` | Optional account configuration; see [ACCOUNTS.md](ACCOUNTS.md). Enables account enrollment and replaces static device authorization. |
| `id` | Unique 1–64 characters: lowercase ASCII letters, digits, `_`, `-`. |
| `name` | UTF-8 display name; 1–64 bytes, no control characters. |
| `secret` | Unique, nonzero, 32-byte hex device authentication key. |
| `allowed_hosts` | Known device IDs, excluding the device itself; no duplicates. |
| `stun_urls` | Optional array of up to 8 `stun:` URLs. |
| `turn.urls` | Array of 1–8 `turn:` / `turns:` URLs. |
| `turn.shared_secret` | 32–256 printable ASCII characters, matching coturn. |
| `turn.ttl_seconds` | Optional credential lifetime, 60–3600; default 600. |

Remove the entire `turn` object if no relay is deployed. Use `stun_urls: []`
if no STUN service is available; discovery will work but ICE will have fewer
connectivity options. Replace example domains with actual services. The server
validates URL schemes but does not probe DNS, certificates, STUN or TURN.

Protect the config with mode `0600` or `0400`, owned by the runtime user or root.
The runtime user must be able to read it. Absolute paths are required; the final
path component cannot be a symlink. Keep parent directories administrator-owned.
Unknown config fields fail validation to catch misspellings.

```sh
chmod 600 /absolute/path/server.json
out/debian/crDeskServer --config=/absolute/path/server.json --check-config
out/debian/crDeskServer --config=/absolute/path/server.json
```

## Install with systemd

The commands below are for the deployment administrator, after building and
preparing the real configuration. They are not run by the desktop installer.

```sh
sudo useradd --system --user-group --no-create-home --shell /usr/sbin/nologin crdesk-server
sudo install -d -m 755 /opt/crDeskServer
sudo install -m 755 out/debian/crDeskServer /opt/crDeskServer/crDeskServer
sudo install -d -o root -g crdesk-server -m 750 /etc/crdesk-server
sudo install -o crdesk-server -g crdesk-server -m 600 /absolute/path/server.json /etc/crdesk-server/server.json
sudo install -m 644 crDesk/server/deploy/crdesk-server.service /etc/systemd/system/crdesk-server.service
sudo systemctl daemon-reload
sudo systemctl enable --now crdesk-server.service
```

If the service user already exists, skip `useradd`. Inspect startup and probe the
local listener:

```sh
systemctl status crdesk-server.service
journalctl -u crdesk-server.service -n 50 --no-pager
curl --fail http://127.0.0.1:9080/healthz
```

The health endpoint returns `{"status":"ok"}`. It proves the event loop/listener
is responding, not that external TLS, devices or TURN are reachable. A second
process configured for the same port fails to bind; systemd manages one process.

## Nginx

Adapt [nginx.conf](deploy/nginx.conf), including the domain and certificate paths,
and include it from Nginx's `http {}` context. Obtain and renew a valid certificate
using your deployment's certificate tooling. The example does not provision
certificates or an HTTP-01 challenge endpoint. Open TCP 443 in the firewall.

```sh
sudo nginx -t
sudo systemctl reload nginx
```

Clients will connect to `wss://desk.example.com/v1/ws`. The example explicitly
forwards the upgrade headers and allows a 75-second upstream idle interval;
clients send application heartbeats every 15 seconds. See the
[official Nginx WebSocket documentation](https://nginx.org/en/docs/http/websocket.html).
Adjust the per-IP limits if many clients share an egress IP. Nginx handshake
limits do not limit frames after an upgrade; the server separately limits
application messages. Keep Nginx and Chromium security updates current.

## STUN and TURN

Adapt [turnserver.example.conf](deploy/turnserver.example.conf) for a separate
coturn service. Set its `static-auth-secret` to the exact same string as
`turn.shared_secret`. This string is used as ASCII bytes, unlike hex-decoded
device keys. Keep the system clock synchronized.

Allow UDP/TCP 3478, TCP 5349, and the configured UDP relay range
49160–49259. Adjust the range and quotas for capacity. If coturn is behind NAT,
configure its public/private address mapping and forward the relay range without
port translation. Ensure coturn can read its certificate and private key.

TURN uses its own listener, not the Nginx HTTP location. A TLS TURN endpoint on
443 for restricted networks needs a separate IP/listener or an explicitly
designed L4 routing setup. Do not put it on the existing WSS socket.

The server issues TURN credentials only to participants in an accepted session,
using the TURN REST HMAC-SHA1 format supported by coturn. Device authentication
uses HMAC-SHA256. See the
[coturn configuration reference](https://github.com/coturn/coturn/blob/master/examples/etc/turnserver.conf).
TURN credentials are bearer credentials valid until expiry; closing a signaling
session does not immediately revoke them or terminate existing relay allocations.
Enforce bandwidth/allocation quotas and monitor usage in coturn.

## Operation and limits

- One active WebSocket per registered device. With `fast_reconnect_v1`, a signed,
  newer connection generation from the same running client replaces its old
  socket immediately after authentication. Different client processes and legacy
  clients cannot evict the existing connection; an orphaned socket from a previous
  process may still require the 45-second idle timeout. See [PROTOCOL.md](PROTOCOL.md#fast-reconnect).
- One pending/accepted incoming session per host, and four outgoing sessions per
  viewer. A host must respond to a request within 30 seconds.
- 64 KiB incoming UTF-8 JSON messages; outgoing discovery lists up to 256 KiB.
  48 KiB decoded signaling payload limit, also
  subject to the encoded envelope limit. At most 256 fragments per message.
- 128 KiB socket receive buffer and 512 KiB pending write buffer per connection.
  Queue overflow disconnects the slow peer instead of dropping signaling.
- Per connection: 64 application messages/s (burst 128), 256 KiB/s (burst
  512 KiB), and six connection requests/minute (burst six). These are abuse bounds,
  not measured throughput guarantees. WebSocket control frames do not count as
  application heartbeats. Connection caps and idle deadlines also apply before
  authentication.
- Authentication/HTTP deadline: 10 seconds; authenticated application idle
  deadline: 45 seconds. A one-second timer enforces deadlines.
- No database, clustering, public registration, browser client, admin API or
  configuration hot reload. Presence and sessions disappear on restart; device
  enrollment remains in the config. No media or signaling payload logging.

To revoke a device, remove its entry and all references to it from
`allowed_hosts`, validate the new config, and restart `crdesk-server.service`.
Rotate its secret to replace credentials. A restart disconnects all signaling
clients; clients must reauthenticate and reestablish sessions. The desktop
integration must terminate affected remote sessions on loss of authorization;
the server cannot itself tear down a direct peer-to-peer media connection.

For an upgrade, stop the service, replace the binary (and matching libraries for
a component build), then start it. Do not overwrite a running executable in
place. To uninstall, disable/stop the service before removing its unit and
binary; preserve the private config only if reenrollment should be retained.

## Manual acceptance checklist

After building, use a WebSocket client implementing [PROTOCOL.md](PROTOCOL.md):

1. Authenticate two registered devices with distinct secrets. Check that only
   allowed targets appear, including offline entries.
2. Publish sharing on one device. Request, accept and relay a small payload in
   both directions; verify that an unrelated device cannot use the session ID.
3. Try a wrong proof and a duplicate connection; verify rejection. Leave a
   request unanswered and verify its timeout.
4. Stop sharing or disconnect a participant; verify a `closed` notification to
   its peer and updated presence where applicable.
5. Request ICE servers in an accepted session; verify an actual coturn allocation
   with the temporary credentials. Rejected/pending sessions must not receive them.
6. Exercise slow readers and oversized/fragmented messages; check bounded queues
   and connection cleanup. Restart the service and verify fresh authentication.

These are deployment acceptance steps, not results of a completed runtime test.
