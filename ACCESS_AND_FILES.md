# Connection approval, pairing, and file transfer

Both endpoints must use this version (signaling version 4, session protocol 4).
Old clients are intentionally unsupported.

## Connection approval

After connection-code authentication, an unknown client waits up to 30 seconds
for the Host's local page to approve it. The Host does not create its WebRTC
session before approval. The local page offers **Allow once**, **Allow and
trust**, and **Reject**. Closing the viewer cancels the request. A stale approval
cannot authorize a later request because every request has a random identifier.
When the UI is closed, the Host shows a native connection-request notification.
Clicking it opens the local UI and the existing screen-card approval bubble; the
notification never grants access itself. Notifications are withdrawn when the
request expires, is answered, or the UI reconnects. The original 30-second expiry
still applies. Windows uses a tray balloon, Linux uses the desktop notification
service, and macOS uses UserNotifications in a bundled application. Notification
permissions and the OS notification policy apply; if notifications are unavailable,
open crDesk manually to approve. A paired device can connect while the UI is absent.

## Trusted devices

**Allow and trust** creates a random 256-bit device credential, separate from the
nine-digit connection code. Credentials are delivered only over the authenticated
WebRTC DTLS data channel. The viewer confirms whether persistence succeeded.
Later connections use a challenge-response proof with that credential. Device
addresses select a saved credential; they do not establish trust.

The titlebar's **Permissions** menu lists devices allowed to access this computer,
with avatars, usernames, trust timestamps, and permission controls. Each device's
**Revoke access** action removes only that client's trust record and disconnects
it if connected. The titlebar's **Trusted devices** menu separately lists remote
hosts this computer has paired with. Both dialogs are available without active
screen sharing and show an empty state when their respective list is empty.

The sharing session's **Revoke access** command clears all trusted clients as well
as rotating the temporary code. Rotating the code alone preserves device trust. The viewer's
**Forget pairing** action, also available in a paired connection prompt, removes
the local credential; it does not revoke the Host's record. If a host address
changes, pair again at the new address.

Credentials are persisted independently of `host.json`: `trusted-clients.json`
and `trusted-hosts.json`. Windows uses the existing user-protected encrypted
storage; Unix uses the private Host directory. No credentials appear in status
responses, discovery, or application logs.

Authentication diagnostics are written by the signaling transport before session
close deduplication. Each connection records its local attempt number, role,
handshake stage, and authentication method (`connection-key`, `pairing`,
`access-token`, or `shared-secret` for internal channels). The host initially
reports `pending` until it reads the client's identity field. Attempt numbers are
local to each transport instance, not shared across the two computers.
Logs never include credentials, identity values, nonces, proofs, or raw messages.

The host's `signaling transport closed` entry distinguishes
`unknown-pairing-identity`, `authentication-proof-mismatch`,
`access-token-unavailable`, `access-token-consume-failed`,
`protocol-version-mismatch`, malformed handshake fields, socket failures, and
timeouts. Connections rejected before the handshake report
`authentication-rate-limit` or `connection-already-active`. A viewer that only
receives EOF reports `peer-closed`; the precise rejection reason stays in the
host log and is not sent over an unauthenticated connection. On Windows, the
ordinary host log is `%LOCALAPPDATA%\crDesk\Crashpad\host\debug.log` under the
account running the host, separate from the UI's `debug.log`.

When the viewer used a saved pairing, authentication failure is shown as a
paired-device authentication error instead of suggesting that the nine-digit
connection key was wrong. This classification does not establish the underlying
cause. It does not erase credentials, retry authentication automatically, or
fall back to another authentication method.

Each trusted client has independent permissions in **Permissions**: remote
control, clipboard Viewer → Host, clipboard Host → Viewer, and file transfer.
Disabling remote control provides view-only access; it does not disable the other
explicit permissions. Changes are persisted and applied to the active session.
The Host enforces input and bulk-data permissions; UI controls are only a reflection
of that policy. Revoking control releases held keys/buttons and rotates the input
epoch. Revoking file access cancels the active transfer and its queued sends.
Clipboard directions also require the sharing session's clipboard mode to allow
them; it remains off by default. Existing trusted records without a permissions
field retain their previous permissions.

## File transfer

Use **Send file** in a connected local or remote tab's context menu. The receiving
side shows the filename and size and requires **Receive file** and an explicit
save location. Trusted devices do not bypass file acceptance. File IO runs on a
blocking-capable sequenced worker, independently of the UI and video threads.

This version supports one active file per connection, up to 16 GiB, plus up to
64 queued sends. The reliable `crdesk-files-v1` channel uses very low priority,
32 KiB binary chunks, and a four-chunk acknowledgement window. Base64 and the
fixed 16 ms per-chunk delay have been removed. Throughput still depends on network,
SCTP pacing and disk speed. Receive queues are bounded and progress updates throttled.
The **File transfers** panel is available in the tab menu and beside inline transfer
status. It shows per-file progress, average throughput since acceptance, estimated
remaining seconds, and the latest 32 completed/failed/cancelled records. Tasks can
be cancelled separately; outgoing failed/cancelled files can be queued again while
connected. Retry starts from byte zero and requires acceptance again. Completed
receives offer **Open folder**. History is in memory; it is not a persistent journal.
Both peers compute SHA-256, and success requires verification and a disk flush.
There is a 60-second acceptance/inactivity timeout and a cancel action.

Existing destination files are never overwritten. Choose a new filename if the
save location already exists. A destination is created exclusively and is visible
while it is being written. Cancellation, normal disconnection, and errors remove
the incomplete destination; a process crash can leave an incomplete file, which
must be removed manually. Received files are not automatically opened. Directory
transfer, resumable transfer, and automatic acceptance are outside this version.
Closing the local UI does not cancel an already accepted Host-owned transfer.

## Clipboard text and images

Clipboard changes use the low-priority bulk channel, independent of input messages.
Text supports up to 1 MiB of UTF-8; PNG supports up to 8 MiB encoded, 8192 pixels per
axis, and 16 megapixels total. Windows, macOS, X11 and the Wayland clipboard Portal
use their native clipboard APIs. A Wayland compositor must expose clipboard Portal
support and grant access. PNG and plain text are supported; rich text, file lists
and arbitrary clipboard formats are not synchronized.

Only changes after enabling are exported. Images take precedence when both text
and PNG are offered. Each direction has one chunk in flight and queues only the
newest subsequent change. Size/dimension checks, ordered offsets, timeouts and
permission gates apply. Disabling a direction or disconnecting discards partial
payloads. Viewer tab activation also gates clipboard delivery. Native errors and
transfer failures surface in the viewer status or Host clipboard tooltip.

## Validation

For this batch, GRIT message gathering, all new translation IDs in the 13 supported
languages, resource references, source paths, and whitespace were checked statically.
No compilation, unit tests, or runtime tests were performed.

Runtime acceptance still required on rebuilt Windows/Linux/macOS endpoints:

- Unknown client: accept once, reject, timeout, cancel while approval is pending.
- Pair, restart both apps, reconnect without a code, revoke one/all credentials,
  forget pairing, and verify rejected credentials cannot reconnect.
- Transfer in each direction: empty file, large file, non-ASCII filename, existing
  destination, cancellation, disconnect, disk error, and incomplete-file cleanup.
- Close the UI during a Host-owned transfer; reopen it and inspect progress.
- Check remote input/video responsiveness while files and clipboard images transfer.
- Queue several files, cancel one, retry a failed send, and open a completed receive folder.
- Close the UI, request access, click the notification, then repeat with timeout/rejection.
- Change each trusted-client permission during a session and after restart.
- Copy long UTF-8 text and PNG screenshots in each direction; check over-limit data,
  disabled clipboard directions, tab switching, and disconnect during transfer.

## Connection profiles and UI

Settings / Connect lets the user choose one of the nine bundled Chromium profile
avatars and a username. Save validates the UTF-8 name (1–63 bytes, no control
characters) and writes `profile.json` atomically in the private Host directory.
Discovery advertisements refresh the public name and avatar on the next announce;
new connections authenticate both profiles as part of the HMAC transcript.
Profiles are display metadata only, never a source of authorization.

The local tab displays the configured avatar to the left of its title. Saving
a new avatar updates the tab immediately. The Host displays a pointed approval
bubble anchored to the currently shared local screen card, with the client's
avatar and username; it does not create a separate incoming-client card. Decisions capture the request identifier and are
dispatched after the button callback returns. Timeout, cancellation, stopping
sharing, and window teardown dismiss the bubble. During a connected session,
both endpoints show the authenticated peer's avatar and username.

The trusted-device dialog uses the About window style: a custom frame, centered
title, 640-by-280 content area, and a centered Close button below a separator.
The scrollable list keeps 48-pixel avatars, names, separators, and delete icons.
Each item's second line shows Trusted and Last active in the local date/time
format. New pairings persist both timestamps; successful authentication updates
Last active without changing the original trust date. Legacy records with no
timestamp show an em dash instead of inventing a date. Host status exports only
profile and timestamp metadata, never pairing secrets. The dialog lists
both clients allowed to access this Host and saved remote Hosts. Revoking an
active Host credential goes through Host IPC. The trusted connection prompt
shows a trusted-device icon, platform and device name with 8-pixel gaps, followed by
Connect with a 22-pixel leading margin, without the previous hint line. The same
22-pixel margin separates the key entry from Connect. Connect receives no initial focus; ordinary
key-based prompts focus the key entry instead. Forgetting a pairing is
managed in the trusted-device dialog.

Rebuild and restart both endpoints, including any background Host, before testing
this handshake version. Verify profile selection, approval/rejection/timeout,
trusted reconnection, removal while connected, and bubble closure during shutdown.

## Device cards and manual addresses

Device cards expose a checked **Add to favorites** item in their context menu.
Favorites show a star in the upper-right corner; there is no favorite button
inside the card. Favorite changes persist across restarts.

The **Add device** icon beside the device-selection label opens a pointed address
bubble. Enter an IPv4 literal and port (`192.168.1.10:47990`). A background IO
sequence probes the remote crDesk listener, with a five-second connection and
response timeout. Only a valid version-4 public profile response adds a saved
manual device and opens its connection-key bubble. Errors keep the address bubble
open; closing it cancels the probe and ignores any late callback. Manually added
endpoints remain in the list when LAN discovery is unavailable. Endpoint matches
merge manual, discovered and favorite records instead of showing duplicate cards.

The probe is unauthenticated public discovery metadata (name/avatar only), not
proof of identity. It neither opens WebRTC nor triggers approval or authentication
failure cooldowns. Administrative sockets do not support probes. Actual access
still requires a connection key and approval, or an existing paired credential.
Both endpoints, including any background Host, must be updated to support probes.
An occupied single-client Host can reject the probe; the UI keeps the input open.

Both paired and key-based connection bubbles display **Connect to** in the title
bar next to the close button. For unpaired devices, the first row shows the
platform and device name with an 8-pixel gap. The second row contains the
Connection key label, key entry and Connect icon. Paired devices show platform
and name alongside the trust icon and Connect, without a key-entry row or hint
line. Missing platform metadata is shown as an em dash. Address-entry and Host
approval prompts retain their respective title-bar text. Key,
address and Host approval bubbles have a close button, retain their arrow, and
remain open on outside clicks or Escape. Completing a connection advances the UI;
request cancellation/timeout and shutdown still clean up obsolete bubbles. Closing
the Host approval bubble with its close button rejects that exact request.

Runtime checks still needed: close/reopen a probe while it is pending, invalid
address/port and unreachable service, successful manual addition followed by key
entry, repeated addition of an existing endpoint, persistence after restart,
favorite toggling by mouse/keyboard, and approval dismissal using the close button.

## Remote toolbar

Remote tabs have an overlay toolbar that can be positioned along the selected edge.
The default is bottom-center. Info toggles the existing connection/GPU display
badges and stays in sync with General / Show connection badges. Full screen
switches the owning window between normal and fullscreen states; the icon also
tracks fullscreen changes made outside the toolbar. Fit and Actual size (1:1)
are available from the remote tab context menu rather than the toolbar.
Open in New Window moves the complete remote surface, including the badges and
toolbar, into a separate window and removes its tab from the main strip without
disconnecting the session. Its caption shows the authenticated peer avatar and
name using the shared crDesk frame (without a tab strip). Its toolbar then offers
Return to tab. Returning or closing that window restores the tab and input focus.
Selecting the same device card again activates its existing detached window.
Detached
surfaces continue receiving frames and accept input while their window is active.
Remote tabs show the authenticated peer avatar and name; the redundant identity
row below the remote image is omitted.

Settings / Interface is the second page and provides the same four icon position
choices as crPlayer (top, bottom, left, right). Padding sits to the right of Position,
with the same 0–36 range and default of 22, and controls the toolbar's edge offset.
An Automatically hide the toolbar checkbox follows this row.
Size appears to the right of Padding and snaps to 22, 36, 48, or 64 logical
pixels per button (default 36); icon size scales with the button. Size changes
apply live and persist along with the other toolbar preferences. While Interface
is selected, remote toolbars stay visible for preview, including detached windows.
Leaving the page or closing Settings restores the saved auto-hide behavior.
Padding and Size sliders have fixed widths of 80 and 50 logical pixels,
respectively; they do not expand to consume the remaining settings-row width.
Top/bottom toolbars are horizontal; left/right toolbars are vertical and hide
the RTT/loss text. Changes apply immediately to all remote tabs and persist in
settings.json. Automatic hiding defaults to enabled. A 12-DIP hot zone along
the configured edge, spanning the toolbar plus an 8-DIP margin at either end,
reveals the toolbar. Leaving the toolbar and edge area
starts a three-second hiding delay; remote desktop mouse/key activity does not
restart it or reveal hidden controls. The gap between the toolbar and the edge
stays interactive for hover retention. Keyboard focus inside the toolbar
prevents hiding; mouse clicks do not request focus. Showing and hiding use a
180-ms opacity transition with an 8-DIP slide toward the configured edge.
Fading-out controls stop hit-testing immediately and become invisible when the
animation finishes. Re-entering the hot zone reverses the animation. Inactive tabs stop their
hide timers, and monitors are detached before the owning window is destroyed.
The overlay does not resize the remote image; hidden controls do not hit-test.

Drag the dotted handle (available in both orientations) or the horizontal
RTT/loss label to reposition the toolbar. Mouse movement must exceed the Views
drag threshold. The toolbar follows the pointer inside the remote surface,
retaining its orientation until release, then docks to the nearest edge.
`remote_toolbar_alignment` stores a clamped 0–1 fraction of available travel
along that edge (default 0.5). Docking and alignment are saved only on release,
using the existing shared toolbar preferences and observer notifications.
Resizing keeps the position proportional. Capture loss, window deactivation,
tab hiding, widget removal, and resizing cancel an unfinished drag. Dragging
suppresses automatic hiding and does not send mouse input to the remote host.

Runtime validation pending: all four positions, changing preferences with an
existing connection, restarting to check persistence, edge reveal and keyboard
focus retention, rapid animation reversal, tab switching, fullscreen via toolbar and menu,
Fit/1:1 at different display scales, and remote input around toolbar controls.
Also validate detaching/returning while another tab is selected, closing a
detached window, closing the main window with detached sessions, peer caption
updates, and live padding changes in both window modes.
Connection prompt titles use approximately 80% of the native title font size.
Connect actions draw a 28-pixel Send icon in a fixed 40-pixel hit target, without
a button background, border or ink-drop fill. Hover smoothly enlarges and tints
the glyph; pressing shrinks it. Localized tooltips, accessible names and keyboard
focus remain available, while the control receives no initial focus.
Device cards show a security badge at the upper right when their address and
port match a locally trusted host. Favorite stars sit beside that badge. The
indicators refresh when the device list changes, the local tab is activated,
or a trusted-device entry is removed from the local page.
Home-page remote device cards are approximately 10% narrower and shorter, with
43-pixel avatars; local screen-preview cards retain their dimensions. A six-pixel
green dot follows the centered device name while it is currently discovered on
the LAN. Saved offline devices have no dot. Platform and address details are
available in the card tooltip rather than a second visible text line.
No C++ build or runtime GUI validation was performed for this change.

## Access tokens

The local sharing header's security icon opens an Access Tokens dialog. Each
selectable token occupies the first line of its item; the second line shows
remaining time and used/maximum authenticated connections. Invalid entries stay
visible in disabled colors. Hovering highlights the full item. Its context menu
offers **Copy Token**, a separator, and **Remove Token**. Copy places the complete
token on the clipboard, even when the displayed text is elided. Remove deletes
the Host-owned token so that future authentications cannot use it, including
handshakes that have not yet completed. It does not disconnect a session that
already authenticated or revoke a separately paired device. Invalid tokens can
also be removed, freeing a slot in the 64-token limit.

The footer controls and dialog caption buttons remain clickable but do not take
focus; token text retains selection and keyboard access. The footer offers 15 minutes, 1/12/24 hours and
2/8/16/32 connections, with 8 pixels between selectors and 12 before Generate
Token. The existing Add device entry now accepts a device access token.

Wire format: `crdesk1:` followed by unpadded Base64URL JSON containing `id`,
`addresses` (IPv4 and usable IPv6 literals), `port`, `key`, `issued_ms` (UTC Unix
milliseconds), `minutes`, and `connections`. Each token has an independent
random 256-bit connection key; it does not export the permanent nine-digit
sharing key. Link-local addresses requiring interface scope and loopback
addresses are excluded. Connection probes try the advertised addresses in
order. The signaling listener uses dual-stack IPv6 with an IPv4 fallback.

The Host owns an in-memory registry for the lifetime of sharing (up to 64
entries). Closing the UI preserves tokens while the Host continues sharing;
stopping/restarting the Host invalidates them. Rotating/revoking the sharing key
revokes all tokens. Changing fields in a copied token cannot extend the Host's
stored validity or connection allowance. Expiry is checked against both wall
and monotonic clocks; it prevents new authentication, not an existing session.
One successful authentication proof consumes one connection, including attempts
subsequently rejected by the Host or failing to establish media. Invalid proofs
and discovery probes consume none. Local connection approval remains required;
tokens do not automatically create trusted-device credentials. Explicitly
trusting a requester still creates the separate existing long-term pairing.

Tokens are not logged or saved in favorites/manual device metadata. Only the
opaque token ID and HMAC proof are sent in the remote handshake, not its key.
The local authenticated administrative channel supplies the token list to UI.
English resources and all 13 supported translations are included.

Validation: GRIT generation, Linux/Windows GN header dependency checks and diff
whitespace checks. No C++ compilation or GUI/network runtime validation.
Pending manual checks: generate/copy/connect, invalid tokens, rejected approval,
use count exhaustion, expiry, IPv4/IPv6 address fallback, reopen UI while Host
stays alive, stop/restart sharing, key revocation, and close dialogs mid-probe.
