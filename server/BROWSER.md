# Browser remote desktop

The account page offers **Connect** beside each registered device. It opens
`/auth/remote.html?device=<id>` in another tab. No device enrollment is needed
for the browser. Sign in to the **same account** as the sharing computer, enable
sharing in that computer's crDesk, enter its connection key, and approve the
request there when required by its existing access policy.

## Supported operations

- WebRTC video, optional audio playback, mouse buttons/movement, scrolling and
  physical keyboard input. H.264 and VP8 are preferred for browser interoperability;
  crDeskServer does not decode or transcode video.
- Fullscreen, a Send key menu, original/automatic/1080p/720p video limits, and a live
  resolution/frame-rate/direct-or-TURN indicator. Video limits do not change the
  host's desktop display mode.
- A centered overlay toolbar in both windowed and fullscreen viewing hides after
  three seconds away from its controls. Move the pointer to the top center or
  activate the top handle to reveal it. Hover, keyboard focus and open menus keep
  it visible. Icons reuse the native Chromium vector paths. The keyboard menu
  sends Ctrl + Space, Ctrl + Alt + Delete, Alt + Tab, Escape and Super / Windows
  using the native release/down/reverse-up sequence. Ctrl + Alt + Delete is
  disabled unless the host advertises `sas`; view-only sessions disable the menu.
- Bidirectional files over the existing `crdesk-files-v1` data channel. The
  receiver explicitly accepts; both ends verify SHA-256 before completion.
  Progress, cancellation and a 60-second inactivity timeout are included.
  To send to the browser, use the sharing computer's existing file-send UI.
  After reception and verification, click **Save file** in the browser.

When the host sends a separate cursor, the browser draws it at the local pointer
position immediately and reconciles it with host feedback. Prediction ends on
blur, leaving the video, loss of control, or after a bounded quiet period so local
mouse activity on the sharing computer can take over. Embedded video cursors do
not use prediction.

The optional `cursor_motion` capability keeps cursor shapes/state on the reliable
input channel and sends position-only `cursor` messages on `crdesk-motion-v1`.
`cursor_sequence` orders updates; `cursor_shape_sequence` associates positions
with a reliable shape/state version. Both are uint64 decimal strings, scoped by
the input `epoch`. Receivers discard stale updates and retain only the newest
position awaiting a shape. The host retains one pending position under congestion
and refreshes stationary positions once a second to recover from packet loss.
Peers without this capability continue using reliable cursor updates. This
optimization requires updated native peers/browser resources; no server binary
or public-port change is needed.

File contents stay on the WebRTC connection (direct or via coturn), not the
account server. The browser allows one transfer at a time and **64 MiB per file**.
It uses memory and Blob downloads, so peak memory can exceed the file size.
It does not implement streaming-to-disk, directory transfer, resume or persistent
browser transfer history. Oversized incoming offers are rejected without reading
file contents. The native client's own file history is unchanged.

## Deployment

Deploy the newly built `crDeskServer`, the **entire** `server/web/` directory,
and the changes in `deploy/nginx-account.conf` together. Use `autoninja` for a
subsequent build; this feature does not require Chromium shared-code changes.
Update the sharing computer's crDesk as well: its protocol-v4 connection profile
must accept the authenticated `persistent_pairing` flag. Cloud access and sharing
must already be enabled. Deploy the updated host and browser resources together;
older hosts reject the extended browser profile.

Nginx must serve `remote.html`, `remote.css`, `remote.js`, `remote-protocol.js`,
`remote-input.js`, and `remote-files.js` with their correct MIME types, alongside
`app.js` and `i18n.json`. Proxy `/auth/remote/ws` with WebSocket upgrade headers,
**preserving Origin and Cookie**. Keep the backend on `127.0.0.1:9080`. The CSP
must allow same-origin scripts, the account-origin WSS endpoint and media Blob
sources. If using a different domain or port, update the example CSP WSS origin
as well as the existing account origin/TLS configuration.

No new public port is needed: HTTPS/WSS use the existing account endpoint;
WebRTC uses existing STUN/TURN services and their relay port range. Use a browser-
trusted certificate for HTTPS and TURN TLS. A native client's custom certificate
verifier does not install trust in a browser.

## Authentication and lifecycle

`POST /auth/remote/ticket` requires the existing authenticated account cookie,
exact configured Origin, and context CSRF token. The response is a one-use,
60-second ticket. The server keeps only its SHA-256 hash and binds it to the
browser account session. Tickets are sent in WebSocket messages, never URLs or
browser storage. At most 128 pending tickets exist and issuance is rate limited.

`/auth/remote/ws` checks the exact Origin and account cookie before upgrading.
After its challenge, the browser sends `authenticate_browser` with the ticket
within ten seconds. The server creates an ephemeral `web-...` viewer, limited to
four simultaneous browser viewers per account. It never issues device secrets
or inserts the viewer into the registered-device database or native presence map.
Native `/v1/ws` continues rejecting Origin headers and cannot authenticate these
reserved temporary identities.

The browser can discover its account's devices and request connections, relay
signaling, obtain short-lived ICE credentials and report session state. It cannot
publish itself as a host, enroll devices or create/redeem cross-account access
tokens through this WebSocket. Host authorization still applies: the browser
implements the native v4 HMAC challenge and signed/ordered signaling using the
host's connection key. Cookie login alone does not grant remote desktop access.
Credentials stay in page memory. The browser includes `persistent_pairing:false`
in its connection profile, covered by the v4 authentication HMAC transcript.
Hosts validate this optional boolean before authentication and carry it through
to the approval UI. A missing flag means the native client's existing persistent
pairing behavior. Browser requests offer only **Allow once** and **Reject**; the
host also enforces this capability when approving and issuing credentials, even
if a trust command reaches it. Pairing secrets are not retained; trusted-only or
unattended hosts reject this unpaired viewer under their existing policy. The
flag does not grant permissions or relax revocation of existing trusted devices.

Revoked/expired browser sessions are disconnected on the next server tick or
message. Closing the tab or losing signaling ends the remote session. A lost
connection requires an explicit reconnect and re-entry of the connection key;
there is no unattended auto-reconnect. Reloading loses in-progress transfers.

## Browser limits and acceptance checks

Start validation with current desktop Chrome/Edge. Other browsers may differ in
codec, fullscreen, keyboard-lock and autoplay support. Audio requires a user
click and an enabled host audio source. Browser/system-reserved shortcuts cannot
all be intercepted; use the Send key menu for its listed combinations. Local IME composition and
clipboard synchronization are not implemented; use the host's input method.

After building and deployment, verify:

1. Sign in, open a device's Connect page, enter its connection key and approve on
   the host. Verify that browser requests offer Allow once/Reject without Trust,
   and stay connected after approval without creating a trusted-device entry.
   Check video, mouse, keyboard, blur/release, audio and resolution limits.
2. Verify wrong keys, another account, offline hosts, disabled sharing, local
   rejection and revoked browser sessions cannot continue controlling the host.
3. Transfer an empty file, a multi-chunk file and a 64 MiB file in both directions;
   verify the saved SHA-256 independently. Reject/cancel transfers and try a file
   above the limit. Revoking file permission must stop an active transfer.
4. Check direct and TURN paths, fullscreen exit, page reload, network interruption,
   host stop-sharing and tab close. Native-to-native connections must still offer
   Trust, persist pairing successfully and reconnect using the trusted credential.

JavaScript syntax and resource consistency can be checked without compiling.
Actual browser/native interoperability and deployed TLS/TURN behavior require
this acceptance run; static checks alone do not establish them.
