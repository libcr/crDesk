# Locked Wayland session capture and input

This optional backend preserves an existing shared session across a confirmed
logind lock. An explicitly authorized native Mojo helper provides KMS capture
and UHID keyboard/mouse devices. Trusted viewers can wake the panel, move/click,
scroll and type on the lock screen when remote control is allowed. The user
still enters their normal login credentials; crDesk does not store passwords or
automatically sign in. This is not boot/login-screen access or access to another
user's session. A sleeping panel keeps the connection alive without frames until
input wakes it. The compositor may consume the first input to dismiss its shield.

Capture uses the verified FFmpeg `kmsgrab` → VAAPI mapping →
8-bit `bgr0` readback path. The Host sends these opaque BGRA pixels through its
existing WebRTC bitmap path. This is not zero-copy and does not preserve HDR.
FFmpeg must support kmsgrab and VAAPI; the GPU driver must support importing the
actual framebuffer modifier and downloading bgr0. Other GPUs are not presumed
compatible. The helper logs conversion errors to the systemd journal.

## Installation

To package an existing Linux x64 build without compiling it, run `make ddeb`.
The default output is `/tmp/crdesk_<version>_amd64.deb`. Override
`CRDESK_LINUX_OUT_DIR` or `CRDESK_LINUX_PACKAGE_DEB` on the make command line
if needed. Packaging requires `dpkg-dev`, `zip`, Python 3, and the strip tool
used by `make ld0`. Dependencies are derived from the existing ELF files on the
build machine; a newer Debian build is not necessarily compatible with older
Debian/Ubuntu releases.

Install with `sudo apt install /absolute/path/to/crdesk_<version>_amd64.deb`.
This installs `/opt/crDesk`, the `crdesk` launcher and an application menu entry.
It includes `crDeskLockHelper` and the setup files, but does not configure or enable the helper;
continue with the steps below for locked-session sharing.

Install the full crDesk package in a root-owned directory such as `/opt/crDesk`.
The executable and every parent directory must be owned by root and not writable
by group/other users. A binary in a user-writable development checkout is
deliberately not accepted by the privileged helper.

Dependencies: Python 3, systemd/logind, polkit (`pkcheck` and a running graphical
authentication agent), libdrm, FFmpeg with kmsgrab/VAAPI, the appropriate
VAAPI driver, and kernel UHID support (`/dev/uhid`). If the UHID module is not
loaded, load it with `sudo modprobe uhid` and configure your distribution to load
it at boot if necessary. Only one connected physical monitor is supported in
this version. The helper pins its EDID, connector and GPU, rejects hotplug/mode
changes, and never chooses another monitor automatically.

Example for the tested machine (verify device paths before installation):

```sh
sudo /usr/bin/python3 -I crDesk/linux/install_lock_helper.py \
  --user user --host /opt/crDesk/crDesk \
  --card /dev/dri/card1 --render /dev/dri/renderD128 \
  --connector card1-eDP-1
```

The native helper is installed beside `crDesk` by default. Use `--helper` only
if its installed location differs. Both executables must be root-owned. Existing
Python-helper installations **must rerun this installer after installing the new
package**: it rewrites the service to run `crDeskLockHelper` and replaces the
polkit prompt to include input authorization. The old socket business protocol
is not compatible with Mojo; the old Python helper is no longer packaged or used.
The installer stops any active helper lease; enable the option again afterward.

For an installed package, invoke the installer at
`/opt/crDesk/crdesk_resources/lock_capture/install_lock_helper.py` with the same
arguments shown above. Packaging needs an existing `crDeskLockHelper` build output;
it fails explicitly if that binary is missing.

Packaged helper files are under `crdesk_resources/lock_capture/`.
The installer writes `/etc/crdesk/lock-capture.json`, installs
`crdesk-lock.service`, `crdesk-lock.socket`, and the
`org.crdesk.lock-capture` polkit policy. Only the socket is enabled at boot;
the root helper starts on a connection and exits when that lease ends. Installing
or upgrading stops any existing helper lease, including the old always-on service.
Installation does not enable sharing by itself. To disable the helper, run:

```sh
sudo systemctl disable --now crdesk-lock.socket
sudo systemctl stop crdesk-lock.service
```

Start screen sharing, pair a trusted device, then enable the locked-session
option under Settings / Connect **while unlocked**, and complete the system's
administrator authentication dialog. The default policy requires fresh
administrator authentication for each lease (no `auth_admin_keep`); local
administrator policy can override this. The option applies to the
current sharing session only. Stopping sharing or selecting a different screen
clears it. Enabling switches the Host to trusted-device-only access, so an
existing untrusted viewer must reconnect as a trusted device.

The initial installation still requires administrator privileges; runtime
authorization cannot install a privileged service for an unprivileged program.
The UI and Host stay unprivileged. The root helper requests polkit authorization
for the kernel-authenticated Host's PID, process start time, and UID before
opening DRM and UHID devices. The main application is never elevated. Systemd's
Unix socket provides activation and kernel peer credentials only. An isolated
Mojo invitation establishes the typed `lock_helper.mojom` interface; there is no
custom command framing or raw HID-report protocol. Frames use read-only shared
memory handles. The helper has no GUI, WebRTC or remote-network listener.

## Lifecycle

- Without the explicit option, Portal failure still ends sharing; KMS is never
  activated. Portal version 2 or newer is asked for application-lifetime
  permission (`persist_mode=1`) during initial screen selection.
- Mojo `Authorize` must succeed before `Poll`, `SetControl` or `Input`. The socket is root-owned and restricted to the configured
  user's primary group; UID, executable and session checks still apply.
- Authentication is bounded to 120 seconds. Cancellation, failure, missing
  authentication agent, lock or session change before approval cannot arm
  capture/input. Stopping sharing cancels bootstrap or disconnects Mojo while
  approval is pending; the helper terminates its pending `pkcheck` process. No late
  authorization callback can re-enable sharing.
- A Portal failure alone never authorizes KMS. The Host allows two seconds for
  the logind lock notification race, then stops sharing if still unlocked.
- The helper independently checks the peer UID/executable and active local
  Wayland seat/session, and checks LockedHint before frame delivery and each
  input event. It accepts bounded typed events, not shell commands, filenames,
  device paths, raw HID descriptors/reports or session IDs supplied by the peer.
- Capture and delivery are capped at 10 FPS. A single persistent FFmpeg process
  is used while locked and frames are demanded; no process is launched per frame.
  CPU/GPU work is off the UI sequence, buffers are bounded, and old output is
  discarded instead of building a playback queue.
- On unlock, the helper stops FFmpeg and returns no more KMS frames. The Host
  recreates Portal using its single-use RemoteDesktop restore token, keeping
  the old remote picture until a valid first frame arrives. The token and bus
  identity survive the lock in memory only; tokens are never logged or saved
  to disk. Each successful restoration replaces the consumed token with the
  newly returned token. Stop sharing or new screen selection discards it.
  Unsupported persistence, a missing/invalid token, revoked permission or a
  changed display can still show the system picker. The backend decides whether
  it can restore permission; the application never suppresses consent.
  Cancellation, dimension mismatch or a two-minute restoration timeout ends
  sharing. There is no automatic return to KMS while unlocked.
- During the KMS phase, keyboard and pointer events go to helper-owned UHID
  devices; stale Portal input and clipboard are disabled. Logical pointer
  coordinates are mapped to an absolute HID mouse; relative mouse activity wakes
  the screen. USB keyboard usages map to a six-key HID keyboard with modifiers.
  Keyboard layout is controlled by the Linux lock screen.
- Input requires a trusted peer and the existing remote-control permission.
  Disabling control, pausing, disconnecting, releasing remote input focus or
  unlocking releases held keys/buttons. Unlocking disables helper input before
  restoring Portal/EIS. A new input epoch prevents old backend events being
  replayed. Re-locking during restoration rechecks the helper state.
- UHID and capture share a worker with nonblocking FFmpeg reads. Only the latest
  complete frame is retained. Input batches are limited to 64 events and one
  outstanding batch; only consecutive pointer motion can be coalesced. Queue or
  validation failures close the lease and destroy its devices.
- Fresh Portal input and clipboard are attached after restoration.
- Stop, helper failure or authorization loss invalidates callbacks. The helper
  closes capture on IPC disconnect/timeout and refuses session/user changes.
  A helper restart requires enabling again while unlocked.

## Manual validation after building

1. With the option off, stop sharing through the system UI: sharing must end.
2. With the option on, stop through the system UI while unlocked: sharing must
   end, with no KMS capture logged.
3. Lock an already connected trusted session and let the panel sleep. Move the
   remote mouse or press a key; verify wake, the lock picture and `max_fps=10`.
   Verify left/middle/right click, wheel, modifiers, text, Backspace and Enter;
   enter a password normally to unlock. Check the lock-screen keyboard layout.
   Repeat with remote control disabled: wake and input must not be injected.
4. Check `Portal restore token received=1` after initial authorization. Unlock
   remotely and check `token_available=1`, `restoring=1`, a new token, and the
   first-frame restoration log. With backend permission restoration support,
   no new picker should appear. Verify mouse/keyboard and clipboard restoration.
   If the backend omits a token or permissions were revoked, complete the
   normal picker (or cancel and verify sharing ends).
5. Repeat locking/unlocking, including locking during the restoration picker.
6. Stop Sharing during KMS, disable the option, stop the helper, unplug the
   monitor and switch users. No old callback may restart sharing or send a frame
   from a different display/session.
7. Enable the option while unlocked: expect system authentication and a pending
   status. Reject it, then retry and approve. Rejecting must leave ordinary
   sharing active but locked-session sharing disabled. Re-enabling after disable
   must request a new authorization under the installed default policy.
8. Hold a key/button, then release focus, disable control, pause or disconnect.
   Verify no stuck keys/buttons. Repeat while unlocking and stopping the helper.
   Check that UHID devices disappear when the lease ends. A view-only trusted
   peer and an unpaired peer must never inject UHID events.
9. While authentication is pending, stop sharing or lock the session. Approval
   must not enable capture. After the lease ends, check that
   `crdesk-lock.service` becomes inactive and `crdesk-lock.socket` remains active.

Diagnostics: `journalctl -u crdesk-lock.service` shows authorization and conversion
failures and the `authorized Mojo lease; UHID devices created` message without logging frames
or passwords. No authentication agent or a missing policy should be investigated
through the polkit journal as well.

These scenarios require hardware/session validation; syntax checks alone do not
establish runtime support.

## Debian package lifecycle

First installation does not enroll users or enable capture. Upgrades and
same-version reinstalls stop sockets before services, update existing generated
templates, and start previously running units subject to local service policy.
Private configuration is preserved; locally edited units and masks are retained
with new templates in `.dpkg-dist` files when applicable. Restart the desktop
application after updating. Restarting the helper ends its interactive lease:
enable locked-session sharing again while unlocked and complete authentication.

Package removal stops/disables services and sockets and removes the polkit
policy, retaining private configuration. Reinstalling after removal does not
enable sharing automatically. Purge removes generated units, policy and system
configuration; personal desktop user data and administrator drop-ins remain.
The reserved boot service account is retained. See `BOOT_ACCESS.md` for boot
mode migration and Portal agent restart requirements.

## Access before graphical login

For reboot, GDM login, logout and persistent desktop access, see [BOOT_ACCESS.md](BOOT_ACCESS.md).
That mode has separate administrator enrollment and must not run alongside the
interactive lock helper. Disable boot access before reinstalling this helper.
