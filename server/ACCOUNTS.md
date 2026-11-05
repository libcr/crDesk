# Google sign-in and passkeys

crDeskServer can own a single-instance account database in addition to its
WebSocket rendezvous service. Nginx serves the small login page and terminates
HTTPS. No additional application service or SMTP server is needed.

## Identity and device access

- Google sign-in uses the authorization-code flow with PKCE, state and nonce.
  The server verifies Google's RS256 ID-token signature and issuer, audience,
  expiry, nonce and verified-email claims. Google `sub`, not email, identifies
  the account. Accounts with matching emails are not automatically merged.
- A signed-in user can add discoverable passkeys to that account. Registration
  requests no attestation and requires user verification. The server uses
  Chromium's FIDO parser and crypto verifier, validates the exact origin and
  RP ID, and stores public keys only. ES256 and RS256 are supported.
- The system browser chooses a local authenticator, password manager, security
  key or nearby phone according to platform support. A phone QR ceremony is
  provided by the browser; crDesk does not generate a substitute QR code.
  **Use a passkey on this device** and **Use a phone** supply the corresponding
  WebAuthn hints. Browsers without hint support show their usual chooser.
- Each installation creates a persistent Ed25519 key for each account-server
  origin. The server derives an account-scoped device ID from the account ID and
  canonical public key. Devices in the same account discover each other.
  Desktop connection approval, pairing keys and control/file permissions remain
  separate checks.
- First device registration requires two explicit confirmations: **Register this
  device** in the authenticated browser, then **Confirm sign-in** in the native
  application after checking the account identity and server address. There is no
  user-entered verification code. Browser approval alone neither creates the
  device record nor issues a credential. The resulting credential is returned
  only to the native client that owns the original random verifier. It is not
  placed in the browser URL, browser storage or page DOM.
- After proving the same installation identity and signing in to the same
  account with Google or a passkey, the client reuses the device record without
  another registration confirmation. Its name, creation time and other metadata remain
  intact. The server replaces the connection credential with a fresh random
  secret and disconnects sessions using the old credential on its next tick.
  A different account, missing local identity or revoked device requires explicit
  registration. Names, email addresses and device IDs alone never prove identity.
- The desktop client persists its credential using the existing private cloud
  configuration storage (DPAPI on Windows). Sign out removes the local
  credential but retains the installation key and last server address. The key
  alone cannot restore cloud access; browser account authentication is required.
  To revoke a device at the server, sign in, use **Manage account**
  in the browser and remove it. Connected revoked devices are disconnected on
  the next server tick. Revocation also invalidates pending native login flows
  for that identity and account. Keep crDesk open for the existing UI-owned cloud broker.

## Domain and Google setup

The deployed origin is `https://auth.wuruxu.cn:1443`; the RP ID is
`auth.wuruxu.cn` (without scheme or port). The hostname must resolve to the server
and the system browser and desktop application must trust its certificate.
An IP certificate does not make IP addresses valid WebAuthn RP IDs.

In Google Auth Platform, create a **Web application** OAuth client, not a
**Desktop application** client. Register exactly:

```
https://auth.wuruxu.cn:1443/auth/google/callback
```

Download its JSON. Its top-level key must be `web`, and `redirect_uris` must
contain the callback above. Keep the JSON and client secret on the server;
do not include them in native packages or the web directory. While Google
Audience is in Testing, add the Google accounts used for testing as test users.
This server requests only `openid email`.

Prepare a separate configuration, preserving the running STUN/TURN settings:

```sh
python3 prepare_account_config.py \
  --google-json /etc/crdesk-server/google-oauth-web.json \
  --base-config /etc/crdesk-server/server.json \
  --output /etc/crdesk-server/server.accounts.pending.json \
  --origin https://auth.wuruxu.cn:1443
```

The tool creates the output with mode 0600 and refuses to overwrite existing
files. Give the service user ownership of the prepared configuration. The
`accounts` block is shown in [accounts.example.json](deploy/accounts.example.json).
Enabling it starts a fresh account namespace: static provisioned devices are
not accepted in account mode. There is no automatic migration or email-based
account linking.

## Deploying the updated program

Use a crDeskServer binary built from the updated sources. Validate it against
the pending configuration with `--check-config` before replacing the current
binary or configuration. This check validates configuration syntax; it does not
perform Google authentication or open the account database.
Run this check as the service user, because the pending configuration belongs to
that user. Deploy the matching build's `icudtl.dat` beside the executable; the
server initializes ICU for URL and HTTPS processing. The updated service unit
also permits AF_NETLINK for Chromium's network-change observer.

```sh
runuser -u crdesk-server -- /opt/crDeskServer/crDeskServer \
  --config=/etc/crdesk-server/server.accounts.pending.json --check-config
```

Run this command only after installing the updated binary, or use the staged
binary's path instead. The old binary does not understand the `accounts` block.

The systemd unit needs `StateDirectory=crdesk-server` and
`StateDirectoryMode=0700`, as in [crdesk-server.service](deploy/crdesk-server.service).
SQLite is stored in `/var/lib/crdesk-server/accounts.sqlite`, with private file
permissions. The parent directory must be owned by the service user with mode
0700. Back up this directory while the service is stopped, along with its
private configuration. The database includes device authentication secrets.

Install `server/web/` at `/opt/crDeskServer/web/`, owned by root, directories
0755 and files 0644. Use [nginx-account.conf](deploy/nginx-account.conf) alongside
an existing IP virtual host if needed. It exposes static assets and `/auth/`
API routes on the login domain, with limits, no access logging and restrictive
browser security headers. The backend remains loopback-only on port 9080.

After checking Nginx configuration and installing the new binary/configuration,
restart crdesk-server and reload Nginx. Update both native clients before testing
Settings → Cloud connection → **Sign in**. Enter the account
server, sign in through the browser, optionally add a passkey, then click
**Register this device**. Return to crDesk, check the account and server shown,
and click **Confirm sign-in**. A second device
signed into the same account should appear in discovery.

## Lifetimes and failure behavior

Browser account logins last a fixed 30 days. The host-only
`__Secure-crdesk-account` cookie uses `Secure; HttpOnly; SameSite=Lax`,
`Path=/auth/`, and `Max-Age=2592000`. It contains a random 256-bit bearer token;
SQLite stores only its origin-bound SHA-256 hash, account, login method, creation
time and absolute expiry. Successful Google login, passkey login or phone
account registration creates a session. Reading or restoring it does not renew
its expiry, and adding a passkey does not extend it. At most 32 remembered
browser sessions are kept per account; older sessions are revoked. Expired rows
are pruned on startup/at most hourly, and expiry is enforced on every use.

A separate `__Secure-crdesk-login` cookie binds a disposable ten-minute browser
context and its CSRF token. Management contexts can be recreated from a valid
account cookie after inactivity or a server restart. The webpage renews these
contexts while visible; it retries only an explicit context/CSRF rejection,
never an ambiguous network failure or a completed WebAuthn ceremony. A retry
must retain the same authenticated account. Browser JSON requests require the
configured exact Origin, browser credentials and a context CSRF token.

Native enrollment requests still expire after ten minutes and remain one-shot.
Google state is single use; passkey challenges are single use and expire after
two minutes. These temporary requests are memory-only and do not survive server
restarts. A remembered browser account can authenticate a fresh enrollment;
new installations still require browser approval and native confirmation.
Native polling uses a separate SHA-256-bound random verifier and a three-second
interval. Pending redemption also checks that the browser account session has
not expired or been revoked. Clearing cookies, using a different browser/profile,
or reaching the 30-day deadline requires signing in again.

For this browser-session change, deploy the rebuilt server binary and matching
web assets together. No desktop-client change is required. Existing cookies
from before this implementation require one new Google/passkey login to create
the persistent session. Verify after deployment:

- Sign in with Google and with a passkey in separate browser profiles; reopen
  **Manage account** and confirm that each profile retains its own account.
- Restart the server and reopen the management page; account login should
  survive, while a pending native enrollment must be restarted from crDesk.
- Leave a management page idle for over ten minutes, then resume an account
  action. The context should recover without another login or duplicate action.
- Sign out on the website; other tabs using that login and pending device
  approvals must lose access. Registered desktop clients remain connected.

Before opening the browser, the native client sends its public key and verifier
challenge to `/auth/device/start`. It signs
`crdesk-device-identity-v1\n<origin>\n<flow>\n<challenge>` with Ed25519 and posts
the signature and verifier to `/auth/device/prove`. The server requires the
canonical Ed25519 SPKI encoding, verifies the signature and binds proof to this
fresh flow and configured origin. Only then can browser authentication begin.
This is installation-key proof, separate from browser WebAuthn passkeys.

`/auth/device/start` advertises `native_confirmation: true`; clients fail closed
if this capability is absent. After browser approval, `/auth/device/poll`
returns `{status:"confirmation", account, email, confirmation}` only to the
client presenting the original verifier. The random confirmation challenge is
bound to this flow and the approved account; it is never exposed in browser
responses. Polling pauses while the native application displays the account
and server for approval. A user click sends `{flow, verifier, account,
confirmation}` to `/auth/device/confirm`, which redeems the credential once.
Early confirmation, a wrong challenge/account/verifier, expired or revoked
flows, and repeated redemption are rejected. Local cancellation discards the
verifier and invalidates queued UI callbacks; unclaimed server flows expire.
An already registered installation can redeem via polling after account
sign-in; that response includes `restoring:true`. The browser distinguishes
approval awaiting native confirmation from a completed server registration.

Native device-credential redemption is one-shot. If the response is lost or
local credential storage fails, sign in again: the verified installation and
account reuse the same record and rotate its credential. Concurrent successful
logins for the same installation/account share one record; the most recently
redeemed credential wins. Managing an existing account uses a separate browser-only flow;
it cannot approve or redeem device credentials. It requires a valid browser
login (Google or a passkey), independent of the native device connection.
The client opens this page using **Manage account** after enrollment. A device
ID in the URL is only a display hint; account ownership is always determined
by the authenticated browser session. See [CLIENT.md](CLIENT.md#manage-an-account)
for metadata, actions and rollout requirements. Logging out clears the browser
cookies, deletes the remembered session from SQLite, and invalidates every
in-memory flow/pending approval bound to that login without revoking devices or
other independently signed-in browsers. If session deletion fails, logout
reports a storage error instead of pretending it succeeded.

Deploy matching desktop, server and web assets together. This protocol requires
installation identity proof and native registration confirmation; there is no
legacy registration fallback or device record migration. Update the server
binary, web assets and desktop clients together for this protocol change. Pre-identity records are not matched by name or email.

Every rename or removal is scoped to the authenticated account, and removal
of the last passkey requires another configured, linked login method. Device
secrets and passkey public-key material are excluded from account-list output.
Management contexts are short-lived but restore the remembered account without
another Google/passkey ceremony. There is no public endpoint that accepts an email address as proof of identity.

Account creation can use Google or **Create an account using a phone**. Phone
registration asks for a nickname, creates a discoverable passkey through the
browser's standard WebAuthn hybrid ceremony, and commits the new account and
its first credential atomically after validation. Nicknames are not unique
identities and do not prove email ownership. The server generates an independent
64-character account ID; passkey-only accounts have no email. Both the browser
and native confirmation display that ID when no email is available.

Existing users must sign in before selecting **Add a passkey on a phone** to
avoid creating a separate account. **Use a phone** signs in with a credential
already saved on the phone. Creation requests use `hints: ["hybrid"]` and
`authenticatorAttachment: "cross-platform"`; assertions use the hybrid hint
without restricting discoverable credentials. Hints are preferences, not proof
that a phone was used. Browsers may still offer another external authenticator,
and unsupported browsers may show their normal chooser. crDesk does not draw a
QR code, access Bluetooth, or bypass user verification. See the
[WebAuthn hint definitions](https://www.w3.org/TR/webauthn-3/#enum-hints).

Keep the phone and computer nearby, online, and with Bluetooth enabled.
The browser/OS supplies the QR code and cross-device tunnel. Successful WSS or
TURN checks do not establish that this tunnel is reachable. Cancellation,
timeouts, unsupported authenticators, and already-registered credentials have
separate page guidance; NotAllowedError cannot identify the exact network or
Bluetooth failure.

Registration and login challenges expire in two minutes and are single-use,
bound to the operation and browser session. Options requests share the existing
server rate limit. An abandoned challenge creates no database account. Failed
atomic writes leave neither an account nor a credential. Existing origin, RP,
UP/UV, key-algorithm, credential-ID, signature and counter checks remain in force.
The native device approval/confirmation protocol is unchanged.

Add a second passkey or security key as a backup. There is no independent email
registration, email recovery or arbitrary cross-provider account linking.
Losing every credential of a passkey-only account leaves no recovery method.
Google sign-in still requires the browser and server to reach Google's endpoints.

After sign-in or account creation, a persistent **Next step** card shows either
native device registration, waiting for native confirmation, completion, or
instructions to initiate enrollment from crDesk when management mode has no
registered devices. Management pages with existing devices hide this onboarding
card. While the device list is loading, the card stays hidden to avoid flashing
an irrelevant registration prompt; filtering the device list does not affect
this decision. Native enrollment guidance remains independent of device count.
The registration button appears directly below the account identity, ahead of
passkey-management actions. Account-creation success messages do not replace
these instructions.

An expired/revoked native flow, a flow already open elsewhere, or browser logout
from that flow requires starting again from crDesk. The page keeps the original
flow URL and hides the generic **Sign in again** action instead of silently
converting a native enrollment into management. Management sessions retain their
website-only sign-in-again action. A `context` URL parameter preserves the UI
recovery hint on reload; it never changes the server's flow type or authorizes
enrollment. An initial flow of unknown type is treated conservatively until the
server answers. Existing passkeys should be used to sign in again, not to create
a second account.

Deploy the server binary, all `server/web/` assets, and rebuilt native clients
together. Older clients require a nonempty email at first-device confirmation
and cannot enroll these passkey-only accounts.

Manual verification after building and deployment:
- On a supported desktop browser, create an account with a phone; cancel once
  and retry. Verify the system QR, phone user verification, account ID display,
  browser approval, and native confirmation before cloud access is granted.
- Sign out and use **Use a phone** with the saved credential. A known installation
  should reuse its device record. A second installation needs confirmation.
- While signed in, add a second credential using **Add a passkey on a phone** or
  **Add a passkey**; both must sign in to the same account and device list.
- Check disabled Bluetooth, missing phone credentials, cancellation and timeout
  guidance. The page must not claim that a QR or phone was used when the browser
  chose a different supported method.
- Confirm Google sign-in, local passkey sign-in, and the last-login-method
  deletion protection still work. Real phone/browser verification is required;
  static checks cannot establish hybrid transport compatibility.

## DNSPod certificate renewal

When public HTTP-01 validation is unavailable, the deployment can use
[dnspod_dns01.py](deploy/dnspod_dns01.py) with Certbot's manual authenticator.
This is automated through auth/cleanup hooks; it does not require an operator
to enter TXT records on each renewal. The hook is scoped to `auth.wuruxu.cn`.

Install the hook at `/opt/crDeskServer/hooks/dnspod_dns01.py` (root, 0700), and
store the DNSPod legacy token as `{"id":"TOKEN_ID","token":"TOKEN_VALUE"}`
in `/etc/letsencrypt/dnspod-crdesk.json` (root, 0600). This is a DNSPod API token,
not a DNSPod account password or a Tencent Cloud SecretId/SecretKey pair. The
server needs Python 3, `dig`, HTTPS access to `dnsapi.cn`, and DNS access to
the zone's authoritative servers plus 1.1.1.1 and 8.8.8.8.

```sh
/opt/crdesk-certbot/bin/certbot reconfigure --cert-name crdesk-auth \
  --authenticator manual --preferred-challenges dns \
  --manual-auth-hook '/usr/bin/python3 /opt/crDeskServer/hooks/dnspod_dns01.py auth' \
  --manual-cleanup-hook '/usr/bin/python3 /opt/crDeskServer/hooks/dnspod_dns01.py cleanup' \
  --non-interactive --no-random-sleep-on-renew
```

Reconfiguration validates against the staging CA before saving renewal options.
It does not replace the active certificate with a staging certificate. The hook
creates a separate TXT record on the explicit DNSPod `默认` (default) line,
waits at least 180 seconds for the zone's
negative-cache TTL and checks authoritative/public DNS propagation. Cleanup
verifies the exact record ID, name, type and challenge value before removing it.
It does not modify existing A, AAAA, MX or other TXT records. The IP certificate
continues to use HTTP-01 and therefore still requires public TCP 80.
Do not substitute a regional line ID for the default line: for this zone,
`10=0` means China Telecom and would hide the challenge from other networks.

## Account tools: history, status and device organization

The account page now includes searchable devices, a group filter, **Edit details**,
**Server status**, and **Activity history**. Each endpoint requires the existing
signed-in browser flow, exact Origin, session cookie and CSRF token. All device
and event queries are scoped to that account; there is no public administrator
endpoint or cross-account device count.

- `POST /auth/account/metadata`: `{id, group, tags, note, order}` updates an owned
  device. Group names use at most 64 UTF-8 bytes; up to eight distinct tags use
  at most 32 bytes each; a single-line note uses at most 256 bytes. `order` is an
  integer from 0 through 9999, with smaller numbers first. Empty group/note/tags
  clear those fields. Metadata changes do not change device credentials or
  remote-control permissions. Connected clients receive a refreshed device list.
- `POST /auth/account/history`: returns `{events:[...]}`, newest insertion first.
  Entries contain a timestamp, category, action and optional subject, detail and
  session ID. Successful account logins, enrollment/restoration, device/passkey
  management, browser sign-out, and signaling session lifecycle are recorded.
  Desktop connection, route and failure observations are explicitly marked as
  client-reported. Browser sign-out is separate from native credential sign-out.
- `POST /auth/account/status`: returns the service feature revision, uptime,
  account-scoped online-device/signaling-session counts, configured STUN/TURN
  transport types, dependency snapshots and device diagnostic summaries.

History is persisted in the account SQLite database, retained for 30 days and
capped at 500 events per account; the page shows the latest 100. Expired entries
are pruned hourly and excluded from queries immediately. Failed login attempts
in an unauthenticated browser flow are buffered in memory (at most eight) and
attributed only if that same flow subsequently verifies an account. Their
original timestamps are preserved. Failures without verified account identity
remain server diagnostics, not another user's account history. WebSocket
credentials, OAuth tokens, SDP, ICE addresses and file contents are not stored
in activity records. Live signaling sessions and temporary enrollment state are
not restored after server restart; remembered browser logins are restored.

Every five minutes, the server checks Google's public-key endpoint (when Google
login is configured) and its configured public HTTPS `/auth/` endpoint, using
normal Chromium certificate verification and a 20-second timeout. The page
shows snapshot time, network/HTTP errors and certificate expiry when available.
These checks do not prove that a full Google login or a TURN allocation works.

Completed native network self-checks can upload a bounded summary over the
already authenticated WebSocket. Only stage/result codes and selected numeric
error fields are stored. Each device keeps its latest report; the page shows
up to 20 most recently reported devices and their timestamps. These are
**client-reported results**, not tests initiated from the server. Removing a
device also removes its latest diagnostic summary.

Group names, tags, notes and order are stored with the existing device record
and survive server restart and device sign-in restoration. The account page
edits these values; native clients show collapsible Cloud subgroups, card tags,
notes in tooltips, and search across all four fields. Grouping is organizational
only: devices of the same account retain the same discovery/authorization rules.

### Device avatars in the account page

Device rows show a 36px circular avatar and a localized "Current device" badge
beside the name. The badge uses the device hint supplied by the native account
management link; it does not grant permissions or identify an arbitrary browser.
Custom avatars fall back to the device's selected built-in avatar when missing
or unavailable. Built-in images match the native client's nine avatar choices.

`POST /auth/account/list` includes `avatar` (0–8) and `avatar_hash`.
`POST /auth/account/avatar` accepts `{id, hash, csrf}` and returns `{hash, data}`
with base64 JPEG data. It requires the existing authenticated browser cookie,
CSRF token, account ownership and a matching current avatar hash. Missing,
revoked, foreign and stale avatars return 404. Responses are not HTTP-cached;
the page keeps bounded Blob URLs in memory and releases them on sign-out.

After rebuilding the server, deploy the complete `server/web/` directory,
including `avatars/0.png` through `avatars/8.png`, and update the Nginx account
configuration. Its avatar static route and `img-src 'self' blob:` policy are
required in addition to the new server endpoint; replacing the executable alone
does not update this UI.
