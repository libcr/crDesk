# Desktop cloud connection

The desktop client supports account sign-in on Windows, Linux and macOS.
Use an account-enabled crDeskServer and update the desktop application on both
computers, including any host process left running by an earlier version.

## Enroll a computer

1. Open **Settings → Cloud connection → Sign in**.
2. Check the account server address (default: `https://auth.wuruxu.cn:1443`).
3. Sign in with Google or an existing passkey in the system browser. Check
   the device name and click **Register this device** when registering for the
   first time. Return to crDesk, check the account email and server address,
   then click **Confirm sign-in**. No verification code is required. A recognized
   installation signing back in to the same account skips this registration step.
4. Wait for **Connected to the cloud server**. Repeat on the other computer
   using the same account; each computer receives its own device credential.
5. On the host, select a screen and enable sharing. Its Cloud entry becomes
   available to other devices registered to the same account.
6. Select that device and complete the existing connection-key and host
   approval/pairing flow. Account sign-in does not bypass desktop permissions.

Manual device JSON import and configuration-provided private CA certificates
are no longer supported. A saved device identity without an account is ignored;
sign in to register the computer. WSS uses normal platform certificate
verification, including certificate chain, validity and hostname checks.

The cloud connection uses direct sockets rather than system HTTP/SOCKS proxy
settings. Normal operating-system routing, VPN routes and firewall rules still
apply; this does not bypass a VPN or select a network interface.

Account-issued credentials are saved in `cloud.json` in crDesk's private data
directory (`~/.crdesk` on Linux/macOS, `%LOCALAPPDATA%\crDesk\data` on Windows).
Windows uses DPAPI protection; Unix uses a private directory and file mode 0600.
On startup, the client reads the saved account credentials and automatically
connects if cloud connection is enabled. Disabling cloud connection retains the
credentials; signing out clears them but remembers the account server address.
To change accounts or servers, sign out of the client first, then use **Sign in**.

A separate `cloud-identity-<SHA256-of-origin>.key` file stores an Ed25519
installation key for each server origin in the same private directory. Windows
protects it with DPAPI; Unix uses mode 0600. It survives sign-out and application
restart. Do not copy this private data directory to another computer: a copied
key represents the same installation, not a new device.

After Google/passkey sign-in, a cryptographic proof from this key lets the server
reuse this account's device ID, name and history and issue a fresh connection
credential. The key alone does not sign in or authorize a new account. Removing
the local key, choosing another account, or revoking the device requires explicit
registration again. A damaged identity file fails sign-in instead of silently
creating a replacement identity.

## Manage an account

Once account credentials are saved, the action becomes **Manage account**,
including while the cloud connection is temporarily offline. It
opens `/auth/` on the saved server in the system browser, independently of any
unsaved server-address edit. It does not start device enrollment, poll for a
credential, or replace the client's existing identity. Browser authentication
is separate: use Google or a passkey if there is no valid remembered browser
session. Successful browser login is remembered for a fixed 30 days in that
browser/profile, including after a server restart. Opening **Manage account**
reuses it; it does not extend the deadline. Clearing cookies, browser sign-out
or expiry requires authentication again. Native client credentials are never
used as browser authentication.

The page shows the account email, current and available sign-in methods,
registered devices and passkeys. Devices show their name, reported platform,
online status, creation/last-online times and a current-device marker when
opened from crDesk. Rename devices or revoke their cloud access. Passkeys show
their name, creation and last-used times, and support adding, renaming and
deleting. Removing the last passkey is blocked on the server when there is no
configured, linked Google login. Signing out of the website clears only the
browser session; it does not sign out the desktop clients.

Device platform is updated by the new client on authenticated connection.
Online status comes from active server connections. Last-online time is saved
on connect/disconnect and at most once a minute while online; crash recovery
can therefore show an earlier timestamp. Older records with no metadata show
**Unknown**, rather than fabricated historical dates. The page refreshes its
lists every 15 seconds while visible and idle, or with **Refresh**.

Update both the client and crDeskServer, and deploy the matching four files in
`server/web/` to Nginx's `/opt/crDeskServer/web/`. Updating the server binary
alone does not update the account page. Existing account records and device
credentials remain valid; no database schema migration is required.

## Connection self-check

Choose **Settings → Cloud connection → Test connection**. The check reports:

- WSS/TLS and device authentication (reuses the authenticated connection when online).
- A fresh authorized-device list; the number includes offline devices.
- STUN server-reflexive address discovery.
- Each configured TURN transport: UDP, TCP or TLS. Two temporary data-only
  peers exchange a request and acknowledgement with relay-only ICE, verifying
  allocation and data forwarding. No remote desktop, microphone or camera is opened.

Up to eight ICE URLs are checked sequentially, with a 12-second deadline per
URL and a 20-second cloud stage deadline. **Stop** or closing Settings cancels
outstanding checks. **Copy report** copies stage results and numeric error codes,
without device secrets, TURN credentials, addresses or SDP. A missing transport
is shown as **Not configured**, rather than a failed test. An empty discovery
list suggests checking that both computers are registered to the same account.

Update and restart **crDeskServer** as well as the client to enable ICE checks.
An older server still supports cloud login and discovery; the check explains
that its STUN/TURN diagnostic endpoint is unavailable. The server permits one
credential check per authenticated connection every 30 seconds. These probes
check this client's path to the configured infrastructure, not reachability to
an arbitrary remote device, NAT compatibility or the remote user's approval.

DNS failures, refused ports, certificate errors, authentication failures,
WebSocket handshake errors and timeouts have distinct messages. Peer connection
requests also distinguish an allowlist denial, an offline/non-sharing device,
and a busy device. A failed TURN TLS probe can require checking certificate
trust: WSS and TURN TLS connect to separate endpoints, so successful WSS
verification does not establish TURN TLS trust. Both use Chromium certificate
verification through crDesk's transport integration. An ICE error alone
does not prove a certificate fault; inspect its code and server logs.

## Transport and trust

The UI process owns one WSS connection shared by all remote tabs and the local
host. Bounded loopback TCP relays carry the existing crDesk authentication and
signaling protocol between that broker and the existing `RemoteSession`s. The
host process receives ICE configuration over its existing Mojo/admin channel
before the relay connects to its listener. No public listener is added for
these relay endpoints.

Server enrollment permits discovery and session signaling; it does not bypass
the host's connection key, peer HMAC authentication, local approval, trusted
client policy, or control/clipboard/file permissions. Cloud pairing records use
the configured server URL plus device ID, independently of a LAN address, and
are never keyed by an ephemeral loopback port. Existing LAN pairings remain
intact; connecting to the same host through the cloud for the first time may
require pairing once more.

Access tokens generated while sharing and connected to an updated cloud server
support both direct and cloud connections. The second line of each token row
shows `LAN + Cloud` or `Direct only`, followed by remaining time and the shared
connection count. If cloud issuance fails, generation reports an error rather
than silently creating a direct-only token. Without a cloud connection, tokens
are direct-only; signing in later does not upgrade previously generated tokens.

To share across accounts, both devices sign in to the **same server**. The host
enables sharing and generates a token; the viewer pastes it into **Connect with
access token**. The client authenticates direct probes using a nonce and the
token key, then falls back to cloud signaling after at most four seconds of
direct probing. Only the configured server is used; an imported token cannot
redirect cloud credentials to another server. An offline viewer or a viewer
signed in to another server receives a sign-in hint if direct probing fails.

The cloud capability authorizes only that host. It never grants access to the
host account's device list or avatars, and the cloud target is not saved as a
discovered device. The host still validates the end-to-end token key and asks
for approval. Direct and cloud authentication consume the same host-owned
counter, once per successful authentication (even if approval is later denied).
Public probes do not consume a connection. Expiration, removal and stopping
sharing prevent new authenticated connections. Removing a token does not close
an already authenticated session; stopping sharing does.

Cloud routing grants are temporary in-memory server state. They survive a
transient WSS reconnect while the host remains sharing, but must be regenerated
after a server restart. The host's authentication tokens also end with its
sharing session. Both clients and crDeskServer need the updated version.

Before peer negotiation, both endpoints request the server's STUN/TURN list and
short-lived credentials. WebRTC tries available candidate paths; traffic goes
directly between peers where possible and through coturn when necessary.
Temporary credentials are refreshed before expiry and the peer's ICE
configuration is updated for subsequent gathering/restarts. WSS carries only
signaling, not screen frames, audio, input events, clipboard content or files.
The normal ICE-path diagnostics identify whether a relay candidate is selected.

crDesk supplies a private PacketSocketFactory for TURN TLS. It uses Chromium's
SSLClientSocket and default certificate verifier on a dedicated IO thread,
without modifying the shared WebRTC TLS implementation. Ordinary UDP/TCP and
WebRTC's STUN/TURN packet framing remain unchanged. The IO thread starts only
when a TURN TLS connection is requested; reads and queued writes are bounded,
and closing a socket cancels outstanding handshake and IO callbacks.

Both real connections and network checks validate the chain, validity, server
usage and the actual server name using Chromium/platform trust. Domain
endpoints require a matching DNS SAN; IP endpoints require a matching IP SAN.
SNI is sent only for domain endpoints. Private CA imports in legacy device
configuration are not used, and insecure TLS options are rejected.

Like the standalone cloud client, the verifier has no certificate network
fetcher. Configure coturn with its full certificate chain. It uses Chromium's
default CRLSet/CT policy rather than WebRTC's older bundled root list.
Connection failures log the stage and a safe Chromium error name, without TURN
credentials. Plain TURN over UDP/TCP can
still carry WebRTC's encrypted media. Do not disable certificate verification
to work around a TURN/TLS trust failure.

The broker sends heartbeats and reconnects with bounded exponential backoff and
jitter. It requests a fresh device list and republishes sharing after reconnect.
When both sides support `fast_reconnect_v1`, the broker signs a process-local
instance and increasing connection generation along with the server's fresh
challenge. A verified newer connection replaces the same process's old socket
without waiting for the server's 45-second idle timeout. An older handshake or
another process cannot replace it. The instance is not persisted, so restarting
the application while its old socket remains on the server uses ordinary retry.
`already_online` is retried rather than treated as an invalid login.

Network-available notifications retry pending reconnects after a 500 ms debounce
instead of waiting for an accumulated backoff of up to 60 seconds. A notification
on a connected broker sends an application ping; it does not immediately end a
working remote session. Pings have a 10-second response deadline, cleared by
incoming JSON traffic. Certificate/authentication failures remain terminal and
signed-out clients do not reconnect. The network notifier runs on the cloud
IO sequence, without adding synchronous initialization to the UI thread.

Sessions from a lost WSS connection are closed, not resumed with stale session
IDs. Reconnect the remote desktop after the cloud connection recovers. Stopping
sharing reports the existing centered sharing-stopped notice to cloud viewers.

An interrupted cloud remote tab now shows a centered reconnecting notice. Once
the cloud broker is online again, it shows a **Reconnect** button, which starts
a fresh authenticated remote session using the tab's existing target and
credentials. The connecting notice remains until the first new remote frame;
it does not claim that cloud recovery has already restored the remote desktop.
Failures are shown in the same badge, and terminal cloud errors direct the user
to Cloud connection settings. Explicit host sharing-stop notices take precedence
and never trigger automatic reconnection. Normal network probes and cloud status
changes unrelated to an active remote tab do not add a centered notice.

## First-version scope

**Keep the desktop UI process running for cloud access**, including on the
sharing computer. Closing the UI terminates the broker and its cloud sessions,
even if an existing unattended host continues running for LAN access. This
version does not enroll pre-login/boot services in the cloud. It does not change
existing LAN or platform unattended-service behavior.

The service cannot guarantee direct traversal through every NAT or firewall.
Configure coturn and its public relay ports for networks that need a relay.
A working WSS connection alone does not prove that WebRTC UDP/TURN is reachable.

## Verification after building

No build or runtime desktop verification was performed for this client change.
After building with the project's `autoninja` workflow, verify with two distinct
computers registered to the same account:

- With both client and server updated, briefly interrupt the cloud network path
  while keeping crDesk running, then restore it. Verify the server logs
  `replacing cloud connection` when the old socket is still present, the client
  returns to `online`, and discovery/sharing recover without the old idle wait.
- Change Wi-Fi or VPN routes while the current cloud path remains usable: the
  connection should remain online after its ping reply. If it is in retry
  backoff, restoring connectivity should trigger an early attempt.
- Run a second process using the same device configuration: it must not evict
  the first. It retries `already_online` without asking for another login.
- Confirm that disabling cloud connection or encountering a certificate failure
  does not cause automatic retries on subsequent network-change notifications.

- TLS succeeds with a trusted certificate and fails with an untrusted certificate
  or mismatched hostname. Account credentials reconnect after restarting the app;
  old configurations without an account do not connect.
- Sign out, restart and sign back in to the same account. The saved server address
  remains, registration is skipped, and the device ID/name/creation time stay the
  same. The device list gains no duplicate. Also exercise Google and passkey login,
  another account, local identity deletion and server-side revocation; these last
  three cases must require explicit registration. An old pending flow must not
  restore a revoked device, and old credentials must fail after rotation.
- Online sharing hosts appear without LAN discovery; stopping sharing removes
  their cloud discovery entry and reports the sharing-stopped notice.
- A wrong connection key is rejected, while approval and remembered pairing
  work normally; file and input permissions still apply.
- A successful remote session logs a selected ICE candidate pair. Verify both
  a direct path and a network configuration requiring TURN.
- Leave a TURN session open beyond the configured credential lifetime and check
  refresh and any subsequent ICE gathering. Inspect both endpoint logs.
- Cancel a connection, sign out of cloud access, restart the server, and reopen the
  app; sessions clean up and fresh device discovery recovers as described above.

## Account sign-in

An account-enabled server supports **Sign in / Manage account** in Settings →
Cloud connection. The default login origin is `https://auth.wuruxu.cn:1443`.
There is no separate **Enable cloud connection** switch. Successful native
sign-in saves credentials and enables cloud connection automatically. Subsequent
launches reuse the saved credentials, and transient network failures reconnect.
**Sign out** stops cloud connection and clears the saved login credentials.
Screen sharing still requires **Enable sharing**; signing in does not enable it.
No migration of previously disabled configurations is performed.

The application opens the system browser for Google or passkey authentication;
click **Register this device** if registration is requested, then check the
account in crDesk and click **Confirm sign-in**. Recognized devices
reuse their registration after account authentication. Each installation gets
its own credential. A public certificate is validated using the application's
normal trust store; configuration-provided private CAs are not supported.

Use **Create an account using a phone** for first registration without Google
or email. The browser supplies the standard Passkey QR ceremony; keep both
devices nearby, online, and with Bluetooth enabled. Choose a phone/tablet in the
system dialog when necessary. Browser and OS support determine available options.
For later sign-ins use **Use a phone** with the saved crDesk credential.
Existing users should sign in first and select **Add a passkey on a phone** to
add that phone to the same account. **Add a passkey** still uses the general
system chooser. Keep a second credential as backup; passkey-only accounts have
no email recovery. Their account ID replaces the email in the browser and native
confirmation. Update the native client as well as the server binary and web
assets before enrolling these accounts.
Devices registered to the same account can discover each other;
sharing and desktop control still require the existing host permissions.
**Sign out on this device** forgets the local cloud credential while retaining
the installation identity and server address. Revoke old or
lost devices in the browser account page. Closing Settings cancels an unfinished
native login. Older clients without account sign-in cannot enroll themselves.

## Account tools

With the server's `account_tools` feature, **Manage account** also opens device
metadata editing, activity history and server status. **Edit details** edits a
device's name, group, tags, note and numeric order together in one dialog and
one save operation; there is no separate device rename button. Passkeys retain
their rename action. After saving, its Cloud card updates through the
normal presence/device-list channel. Cloud subgroups can be collapsed, search
matches metadata, and unavailable devices remain visible with their existing
availability indicator. Empty groups appear directly under Cloud. Favorite
metadata is cached locally, while the server remains the source of truth.

The client reports actual connected/disconnected transitions, a bounded summary
of direct/relay route changes, terminal failure codes and completed network
self-check results. These reports are scoped to the authenticated device/session
and never include media, clipboard contents, file contents, access tokens or raw
ICE/SDP. Reports are best effort: if the cloud connection is down, history may
contain only the server-observed closure. Existing clients without this feature
continue to connect but do not supply these extra observations.

## Custom device avatar

In **Settings → Connect**, click the avatar to select a built-in image, choose a
PNG/JPEG, or restore the default. Imports accept up to 8 MiB and 4096×4096 pixels.
The client honors EXIF orientation, center-crops to a square, flattens transparency
onto white, and encodes a 256×256 JPEG of at most 24 KiB. Only this thumbnail is
shared; original metadata and the original file are not uploaded. The display is
circular, with the existing built-in image as the fallback.

Cloud discovery synchronizes the thumbnail through the authenticated device
connection when `avatars_v1` is available. LAN mDNS publishes only `avatar_hash`;
the public profile probe fetches missing thumbnails separately from the advertised
LAN endpoint. Like device names, LAN avatars are public discovery metadata and do
not authenticate the peer. A busy host may defer its public probe; the authenticated
connection can also fetch the avatar using MAC-protected messages. Connection
profiles remain small (name, built-in avatar, optional hash), including the trust
store and authentication transcript.

Thumbnail files are cached as `avatar-<sha256>.jpg` beside the local profile:
`~/.crdesk` on Linux/macOS and `%LOCALAPPDATA%/crDesk/data` on Windows. The cache is
limited to 256 files (at most 6 MiB of image data); the selected local image is
retained. Missing/invalid images fall back to the built-in avatar and can be fetched
again. Device cards, tabs and peer identity dialogs refresh when an image arrives.
The separate pre-login service may continue showing its built-in avatar when it
cannot access the user's image cache.
