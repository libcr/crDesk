# Boot, GDM login and user desktop access (Linux)

This is a separate, persistent administrator authorization. Installing a deb or
checking **Keep sharing when this Wayland session is locked** does not enable it.

## Supported configuration

- Debian with GNOME/GDM, **Wayland for both the greeter and user desktop**.
- One connected physical monitor, one GPU with working FFmpeg kmsgrab/VAAPI,
  and one explicitly selected non-root desktop user.
- Existing crDesk paired viewers. Pair and select **Remember** in ordinary
  sharing first; boot access has no temporary connection-code fallback.
- UHID kernel support. The installer loads `uhid` and installs a modules-load
  entry so it also loads at boot.
- Network must be available before graphical login. A user-session-only VPN or
  Wi-Fi secret unlocked by the user's keyring cannot provide pre-login access.

This mode provides video (KMS up to 10 FPS, Portal up to 30 FPS, software WebRTC encoding) and
keyboard/mouse. Audio, clipboard, files, multiple monitors, other users, X11,
headless virtual displays and pre-OS/full-disk-encryption prompts are outside
this mode. Login/unlock uses the ordinary GDM/lock-screen password prompt;
crDesk does not store a Linux login password or enable automatic login.

## Architecture and authorization

`crdesk-boot.service` runs the installed crDesk executable as a dedicated
`crdesk-boot` account, with no root capabilities, DRM access or session bus.
It initializes only the headless host runtime and accepts the enrolled viewers
using the existing authenticated signaling and encrypted WebRTC protocol.

`crdesk-boot-helper.socket` activates `crDeskLockHelper --boot-helper` as root.
The Unix socket bootstraps an isolated Mojo invitation; every command and frame
uses `lock_helper.mojom`. Socket credentials, executable path, PID start time and
membership in **crdesk-boot.service** are checked before accepting Mojo traffic.
Only this helper opens the privileged KMS/UHID devices or starts the fixed
FFmpeg capture command. The Portal agent uses normal user GPU access.

The helper checks logind's active local Wayland session on seat0 for every poll
and input batch. Only the configured GDM UID with class `greeter`, or the selected
user UID with class `user`, is eligible. Other users and virtual terminals
suspend capture and input. Console/lock/geometry changes release held input,
stop the capture child, increment an input generation, and discard stale frames.
The client also changes its network input epoch. Stale input cannot intentionally
be replayed into the next generation. As with a physical input device, a desktop
transition concurrent with an individual kernel input write is not atomic.

The same unprivileged Host retains the WebRTC connection through login, logout
and lock/unlock. GDM and locked sessions use KMS/UHID. After the selected user
logs in or unlocks, the Host attempts Portal/EIS through a user-session agent.
The agent starts through GNOME autostart, using the user's own Wayland and
session bus. It carries no network listener or machine pairing credentials.

`crdesk-boot-portal.socket` is a root-owned systemd listener passed to the boot
Host. The Host validates the connecting UID and matches the agent's logind
session against the root helper's current unlocked-session result. The socket
only bootstraps Mojo; frames use bounded read-only shared memory and commands
use `BootPortalAgent` / `BootPortalRegistry`. No custom socket command protocol
or root process is added to the user desktop.

KMS/UHID remains available while Portal permission is pending. Switching requires
a full-size Portal frame matching the enrolled display, EIS readiness and a
helper acknowledgement that UHID control is disabled. Frame and input generation
checks discard old traffic. Lock, logout, agent disconnection or Portal failure
falls back to KMS/UHID. A failed Portal attempt is not repeatedly retried within
the same console generation; an unlock/new session or agent reconnect can retry.

Restore tokens remain in the agent's memory and reuse the same session bus for
lock/unlock. A fresh login/reboot, expired token or backend policy can still show
a consent dialog. KMS/UHID stays usable so the viewer can explicitly approve it;
crDesk never automatically approves consent. Cancelling or timing out leaves
KMS/UHID active. If the autostart agent is absent, boot access still uses KMS.

Portal frames are copied through shared memory to the persistent system Host.
This restores Portal capture and EIS input, not the ordinary Host's zero-copy
GPU encoding, audio, clipboard or file-transfer features.

Updated viewers show `KMS/UHID` or `Portal/EIS` in the remote performance badge,
following the Host's current backend. Enable performance statistics to see it.
The label is separate from the viewer's CPU/GPU display status. A Windows viewer
must be updated to display this new field; older viewers ignore it and remain
compatible. Disconnection or a Host without backend reporting hides the label.

Monitor EDID, connector suffix and physical GPU sysfs path are enrolled. DRM
card/render numbers are resolved again at helper startup so renumbering on boot
is supported. Unsupported monitor changes suspend access. During a transition,
panel power-off or absent frames, the Host sends black frames rather than leaving
the prior desktop visible. Moving the remote mouse or pressing a key can wake
an authorized console through UHID; it does not bypass the login password.

## Install and enable

Build/package using the project's normal `autoninja` workflow and `make ddeb`,
then install the deb. Both `crDesk` and `crDeskLockHelper` must come from the same
build because their local Mojo interface changed. The Windows viewer's network
protocol has not changed.

1. While logged in, pair the intended Windows viewer with normal sharing and
   remember it. Keep the same Linux address and port for subsequent boot access.
2. **Stop sharing** in crDesk. Closing the window can leave the ordinary Host
   listening, so closing the window alone is insufficient.
3. Run the installed, root-owned setup script explicitly as administrator:

```sh
sudo /usr/bin/python3 -I \
  /opt/crDesk/crdesk_resources/lock_capture/install_boot_access.py \
  --enable --user user --host /opt/crDesk/crDesk \
  --card /dev/dri/card1 --render /dev/dri/renderD128 \
  --connector card1-eDP-1
```

The default greeter account is `Debian-gdm`; use `--gdm-user gdm` on a system that
uses that account name. The default signaling port is 47990. A different port
requires pairing on that same port first, because viewers index credentials by
host address and port. Use the original address, not a new hostname/IP alias.

The script copies a validated snapshot of `~/.crdesk/trusted-clients.json` and
the connection profile into `/etc/crdesk/boot-access.json` (root-owned, 0640,
readable by the dedicated service group). It installs/enables the boot units and
disables the interactive lock-helper socket to prevent competing input leases.
Do not re-enable the ordinary lock helper while boot access is active.

The installer also creates `/etc/crdesk/boot-portal.json` (public UID/enabled
metadata only), the Portal socket unit and
`/etc/xdg/autostart/crdesk-boot-portal.desktop`. On an existing installation,
rerun the above `--enable` command after installing the updated package so these
files are installed. The agent starts automatically at the next GNOME login.
To start it in the current graphical session, run as the selected user, without
sudo:

```sh
/opt/crDesk/crDesk --host-process --boot-portal-agent
```

The agent does not open a second signaling port. Close this manually started
agent after testing; subsequent logins use autostart. Re-enabling a previously
disabled setup in the same login also requires starting the agent again.

The machine enrollment is **separate from the user's Settings trust list**.
Revoking a device in ordinary Settings does not revoke this root-owned grant.
After pairing/revoking devices locally, explicitly refresh the machine list:

```sh
sudo /usr/bin/python3 -I /opt/crDesk/crdesk_resources/lock_capture/install_boot_access.py --refresh-trust
sudo /usr/bin/python3 -I /opt/crDesk/crdesk_resources/lock_capture/install_boot_access.py --status
```

For immediate machine revocation, use an ID from `--status` (never a secret):

```sh
sudo /usr/bin/python3 -I /opt/crDesk/crdesk_resources/lock_capture/install_boot_access.py --revoke DEVICE_ID
# Revoke every machine-enrolled viewer and stop the services:
sudo /usr/bin/python3 -I /opt/crDesk/crdesk_resources/lock_capture/install_boot_access.py --revoke all
```

Refreshing or revoking stops existing sessions before updating the configuration.
A helper also terminates its lease if the root-owned configuration changes or
is removed. No trusted-device secrets or keyboard contents are logged.

## Disable, upgrade, remove

```sh
sudo /usr/bin/python3 -I /opt/crDesk/crdesk_resources/lock_capture/install_boot_access.py --disable
```

This stops/disables the boot service and both sockets, stops the helper, releases
UHID devices and removes the machine enrollment, agent authorization and autostart entry. It does not delete the user's ordinary pairings.
Root-owned unit files, the system account and the UHID modules-load entry remain
inert for later reuse. To return to interactive lock-only sharing, rerun
`install_lock_helper.py` following `LOCK_CAPTURE.md` after disabling boot access.

The deb package handles upgrades, including same-version reinstalls: it records
running units, stops activation sockets before services, refreshes deployed
templates and starts previously running units subject to local service policy.
Connections are interrupted. Machine enrollment and trusted-device snapshots are
preserved; no enrollment command or renewed machine authorization is needed.
Disabled units are not enabled. An upgrade also adds the Portal socket/agent
templates to an existing enabled boot installation. Locally edited files and
systemd masks are preserved; review `.dpkg-dist` files for manual migration.

Restart an already-running per-user Portal agent after updating (or log out and
log in again); desktop processes are not terminated by the package scripts.
KMS/UHID remains the boot service fallback while a new Portal agent is unavailable.

`apt remove crdesk` stops/disables the services and sockets, removes the Portal
autostart/public grant and polkit policy, and retains private machine enrollment.
An existing Portal agent exits when its public grant disappears. Reinstalling
after removal does not reactivate access: run the explicit enrollment procedure.
`apt purge crdesk` additionally deletes generated units, machine configuration,
the UHID modules-load entry and `/var/lib/crdesk-boot`. It preserves desktop
users' data, administrator unit drop-ins and the reserved service account (to
avoid reusing its UID). Unrelated files under `/etc/crdesk` are left alone.

For portable/custom installations, stop both sockets before services and rerun
the explicit installer after replacing binaries. The deb only refreshes
enrollment whose host path is `/opt/crDesk/crDesk`. Package installation never
silently creates or broadens enrollment.

## Runtime acceptance checks

These checks require the real GNOME/GDM, GPU, UHID and Windows viewer; parsing
or building alone does not verify them.

```sh
systemctl status crdesk-boot.service crdesk-boot-helper.socket crdesk-boot-portal.socket
journalctl -b -u crdesk-boot.service -u crdesk-boot-helper.service --no-pager
```

- Reboot and leave Linux at GDM without logging in locally. The enrolled viewer
  must connect, see GDM, wake it and enter credentials through remote input.
- After login, retain the connection and receive the selected user's desktop
  over KMS while any Portal consent is pending. After approval, look for
  `capture path=Portal/EIS; first frame ready, UHID released` in the Host journal.
  Repeat lock/unlock and verify `restoring=1` when the backend returns a token;
  a new login may require consent again.
- Cancel the Portal dialog or stop the user agent: the existing viewer must keep
  KMS/UHID access. Test logout and panel power-off/wake on both paths.
- Hold a key during a transition/disconnect; it must not stay held in the next
  session. Old-epoch input must be ignored.
- Switch to a different user or a text VT: output must become black and input
  must be disabled. Returning to the enrolled user/GDM must resume access.
- Unpaired viewers must fail authentication. Revoke an enrolled device while
  connected; the connection must end and reconnect must fail.
- Stop/kill the helper, test temporarily unavailable GPU/FFmpeg and unplug/change
  the monitor. The Host should black the screen, disable input and retry safely.
- Confirm both services start after a second reboot, and `--disable` prevents
  access both immediately and after reboot.

If the greeter is X11, multiple monitors are connected, or kmsgrab cannot read
this GPU's scanout, this implementation will suspend/retry rather than capture
an unintended desktop. Review helper journal errors before changing enrollment.
