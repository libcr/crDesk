# crDesk

crDesk is a remote desktop application for Windows, Linux, and macOS. Share a
screen, control another computer, exchange files, and reconnect to devices you
trust. It uses Chromium and WebRTC, with hardware video acceleration where
supported.

The **host** is the computer sharing its screen. The **viewer** is the computer
connecting to it. The same application can serve either role.

For the standalone discovery/signaling server and Nginx deployment, see
[crDeskServer](server/README.md). To connect computers across networks, configure
**Settings → Cloud connection** on each computer; see the [client setup guide](server/CLIENT.md).
Keep the desktop application open for cloud access.

## Features

- Screen sharing and remote keyboard and mouse control, including view-only access.
- Local network discovery, manual device addresses, and favorites.
- Connection keys, temporary access tokens, and remembered device pairing.
- Separate permissions for remote control, clipboard directions, and file transfer.
- File transfers with progress, cancellation, queuing, and integrity verification.
- Plain-text and PNG clipboard sharing, plus system audio and microphone controls.
- Remote desktops in tabs or separate windows, with fullscreen, Fit, and Actual size views.
- Performance badges showing the active video codec, frame rate, latency, and display path.
- Optional automatic sharing and login-screen access, depending on the host platform.

## Screenshots

**Device discovery** — the main window on Linux, with local sharing controls,
discovered LAN devices, and cloud connection status.

![crDesk on Linux showing local sharing controls and LAN and cloud device lists](Screenshot%20From%202026-11-05%2017-58-32.png)

**Remote desktop** — a Windows desktop open in a crDesk tab on Linux.

![crDesk on Linux displaying Windows Settings in a remote desktop tab](Screenshot%20From%202026-11-05%2017-59-30.png)

## Installation

Use a package that matches your operating system and architecture. Keep the
application's bundled libraries and resources together. Update both computers
when changing versions; incompatible versions cannot connect.

### Windows

Extract the Windows x64 ZIP into a folder and run **crDesk.exe**. Keep the folder
at a stable location if you plan to enable automatic sharing.

For lock screens, UAC prompts, administrator windows, or access before sign-in,
see [Windows automatic sharing](#windows-automatic-sharing).

### Debian Linux

Install the AMD64 deb package with APT so that dependencies are resolved. For
example, replace the filename below with the package you downloaded:

```sh
sudo apt install ./crdesk_1.0.0_amd64.deb
```

Launch **crDesk** from the application menu, or run:

```sh
crdesk
```

The deb installs the application under `/opt/crDesk`. Run the desktop application
as your normal user. Installing the package does not start sharing or enable
unattended access.

A Linux ZIP can also be extracted and launched using `./crDesk` from its folder.
The privileged lock-screen and boot-access helpers require a separate
administrator setup in a protected installation location.

Linux package compatibility depends on the distribution and libraries used to
build it. A package built on a newer Debian release may not run on an older one.

### macOS

Place **crDesk.app** in Applications or another stable location and open it.
Screen sharing requires **macOS 14 or later**; a particular package may require a
newer version.

Allow screen recording when prompted. Remote keyboard and mouse control also
requires **Accessibility** permission in System Settings. Microphone and system
audio sharing may require additional permissions. **Settings → Audio** shows
permission status and links to the relevant system settings. Reopen crDesk if
macOS requires it after a permission change.

## Connect for the first time

Both computers must be reachable over the network, such as a local network or a
VPN that allows traffic between them. Local discovery finds nearby devices; it
does not make an otherwise unreachable computer accessible over the internet.

1. Open crDesk on both computers.
2. On the host, select **Choose screen**, select the display to share, and approve
   any operating-system screen-sharing request.
3. Once sharing is active, find the host's **Connection key**.
4. On the viewer, select the host in the device list. If it is not discovered,
   use **Add device** and enter its IPv4 address and port, such as
   `192.168.1.10:47990`. The default port is **47990**.
5. Enter the host's nine-digit connection key and select **Connect**.
6. On the host, choose **Allow once**, **Allow and trust**, or **Reject** when the
   request appears. Requests expire if they are not answered.
7. After connecting, click the remote picture to control it when remote control
   is allowed.

**Closing the host window can leave sharing active. Use Stop to end ordinary
screen sharing.** Reopening crDesk lets you manage the existing session. On
macOS, closing the main window hides the application; the menu-bar icon remains
available.

## Trust and access controls

Choose **Allow once** for a single approved connection. Choose **Allow and trust**
to pair the viewer for future connections without entering the temporary key or
approving each connection locally.

Open **Permissions** from the titlebar menu to review devices allowed to access
this computer, revoke a device's access, or change its permissions:

- Remote control.
- Clipboard from viewer to host.
- Clipboard from host to viewer.
- File transfers.

Turning off remote control makes the connection view-only; clipboard and file
permissions remain separate. Revoking an active viewer disconnects it.

Open **Trusted devices** from the titlebar menu to review remote computers you
have paired with and use **Forget pairing** to remove a saved pairing. Both dialogs
are available when screen sharing is stopped.

**Change key** rotates the temporary key but keeps trusted devices.
**Revoke access** also removes trusted clients. **Forget pairing** on a viewer
removes that viewer's saved credential; it does not remove the host's trust record.

Saved pairing is associated with the host address and port. If either changes,
you may need to pair again at the new address.

### Temporary access tokens

Use the security icon in the local sharing header to open **Access Tokens**.
Generate a token with a time limit and connection allowance, then give it to the
intended viewer. The viewer can paste it into **Add device**.

Tokens still require local connection approval and do not automatically make
the viewer trusted. Stopping sharing or changing the sharing key invalidates
the tokens. Expiry prevents new connections; it does not disconnect an existing
session. Treat a token as an access credential when sharing it.

## During a session

### Viewing and control

Use the remote toolbar to toggle **Full screen** or choose **Open in New Window**.
Use the remote tab's context menu for **Fit** and **1:1**. A detached desktop can be
returned to its tab. Toolbar position, size, padding, and automatic hiding are
available under **Settings → Interface**.

Use the **Streaming resolution** button in the remote toolbar to choose:

- **Auto (default)**: adapt resolution to connection conditions.
- **Original resolution**: keep the shared screen's pixel dimensions; frame rate
  and bitrate can still adapt.
- **Up to 1080p**: fit within 1920 × 1080, or 1080 × 1920 for a portrait screen.
- **Up to 720p**: fit within 1280 × 720, or 720 × 1280 for a portrait screen.

The limits preserve the aspect ratio and never enlarge a smaller picture.
The capped modes may reduce resolution further when needed. The choice is saved
for each device and restored on reconnect. This changes the transmitted video;
the host's display resolution stays unchanged. The button is disabled until
connected to a host that supports this feature. Update both computers to use it.

Drag the dotted handle to move the toolbar. Release it near a
window edge to dock it there; left and right docking use a vertical layout.
Its position along the edge is saved across restarts and scales with the window.
This preference is shared by remote tabs and detached windows. Dragging keeps
the toolbar visible; losing mouse capture cancels the move.

With automatic hiding enabled, move the pointer to the window edge directly
beside the toolbar's saved position to reveal it. It fades and slides away about three seconds after you
leave the toolbar and its edge area, even while you keep using the remote
desktop. Keyboard focus within the toolbar keeps it visible. Left and right
layouts show icons only; top and bottom layouts also show network statistics.

Use the keyboard button (**Send key combination**) to send **Ctrl + Space**,
**Ctrl + Alt + Delete**, **Alt + Tab**, **Escape**, or **Super / Windows** to the
remote computer without pressing the combination locally. The toolbar stays
visible while this menu is open. Remote control must be available; **Ctrl + Alt +
Delete** is disabled when the host does not support it.

The host can pause sharing, change the selected screen, or turn remote control
off. The viewer can disconnect without stopping the host's sharing session.

### Clipboard and files

Clipboard sharing is **off by default**. Select the permitted direction or
**Both directions** to enable it. Trusted-device permissions must also allow that
direction. Plain text and PNG images are supported; file lists and rich text are
not synchronized. Wayland clipboard support depends on the desktop portal and
its permissions.

On the local tab, use the **Send file** icon after the audio and microphone controls once a remote
device connects and file transfers are permitted. You can select multiple files.
You can also choose **Send file** from a connected remote tab's context menu. The recipient must
select **Receive file** and choose a save location, even for a trusted sender.
On the host's local tab, incoming requests appear in a bubble pointing to the
connected device's avatar. Closing the bubble declines the request.
Open **File transfers** to see progress, cancel tasks, retry failed sends, or
open the destination folder after completion.
Each transfer has a circular action button on the right, with transfer progress
shown around its border. Incoming requests offer only **Receive file**. During
a transfer the button cancels it; after completion it opens the received file's
folder, or shows a check mark for a sent file. Failed or cancelled sends offer
retry while their original session still retains the source file. Hovering over
a row highlights it, and button tooltips describe each action.
Completed, cancelled and failed transfers are saved automatically, with up to
100 records for the local host and 100 for each remote address/port (cloud records
use the server URL and device ID). Records include the filename, direction,
size/progress, completion time and the destination of received files. Reopen
**File transfers** after restarting to view them or open a received file's folder.
Restored records do not resend files or replay completion notifications. Source
paths are not persisted, so retrying an old send after restart requires selecting
the file again. Pending transfers interrupted by a normal disconnect or shutdown
are recorded as failed; abrupt termination may lose unfinished records.
History is stored under `transfer-history` in the private crDesk data directory
(`~/.crdesk` on Linux/macOS, `%LOCALAPPDATA%\crDesk\data` on Windows). Windows
records use DPAPI; Unix directories/files use modes 0700/0600.
Transfer status and actions appear in a dismissible overlay at the bottom of
the relevant tab or detached desktop window. The overlay does not resize the
page or desktop, and closing it does not cancel the transfer.
Completed transfers also show a notification at the bottom of the relevant tab
or detached desktop window. Received files offer **Open folder**; sent files and
combined completion notices offer **File transfers**. The notification fades out
after five seconds of viewing, with the timer paused while hovered or focused.
It does not take focus or switch tabs, and can also be dismissed with its close button.

Transfers support individual files up to **16 GiB**, with one active transfer per
connection and queued outgoing files. Existing files are not overwritten.
Folders and resumable transfers are not supported; a retry starts from the
beginning and requires acceptance again.

### Audio

After connecting, use the audio controls to share the host's system sound or
enable a microphone. Select input and output devices in **Settings → Audio**.
**System audio echo cancellation (AEC)** is off by default to reduce CPU usage.
Enable it on the sharing computer if remote audio is being sent back through
system audio sharing. Changes apply to active sessions and are saved for future
sessions. Microphone echo cancellation is unaffected.
Availability depends on the platform, devices, and system permissions. An audio
permission failure does not by itself end screen sharing.

### Performance badges

Local screen previews use at most 640 × 360 pixels and approximately 5 FPS.
They stop updating while their tab or window is hidden or minimized; this does
not change the preview preference or the resolution sent to a remote viewer.

Hidden or minimized remote tabs skip display conversion and delivery. When both
ends support background video control, the host also limits video to 1 FPS and
restores the normal rate when the viewer becomes visible again. Detached viewer
windows follow their own visibility. Audio, file transfers, and the connection
remain active. Older hosts keep their existing sending rate.

Use the toolbar's Info control or **Settings → General → Show connection badges**.
The badges show display mode, encoding and decoding information, resolution,
bitrate, received frame rate, and connection metrics.

Compact labels keep the overlay narrow: **Enc/Dec** means encoding/decoding,
**HW/SW** means hardware/software processing, and **E/D avg** shows average
encoding/decoding times in that order. **Lost Σ** is the total number of lost
packets. Resolution and bitrate show their values directly, and **Target**
shows the current target frame rate and resolution scale. Labels are localized.

For a Linux host that reports its sharing mode, the first row combines the
viewer's **GPU display** or **CPU display** label with one of these modes:

| Mode | Meaning |
| --- | --- |
| **Portal/EIS** | Screen capture and input through the Linux desktop's authorized sharing interfaces. |
| **KMS/UHID** | Screen capture and virtual keyboard/mouse input through the explicitly configured Linux helper. |

GPU display describes how the viewer presents the image. It does not mean that
the host is using a GPU encoder or that every stage avoids copying frame data.
Check the encoding and decoding fields separately.

The video codec depends on both devices and the capabilities enabled in their
builds. **Linux hosting currently does not enable HEVC encoding**, even when the
GPU supports HEVC decoding. Seeing H.264 on a Linux-hosted connection is expected.
Actual frame rate depends on capture, conversion, encoding, and network speed;
the target frame rate is not a guaranteed delivery rate.

## Automatic and unattended sharing

These features require explicit setup. Pair the intended viewer before relying
on access without someone at the host. Login and unlock still use the host's
normal operating-system credentials.

### Windows automatic sharing

Start sharing, pair a trusted viewer, then enable
**Settings → Connect → Start sharing automatically after Windows sign-in**.
Automatic sharing accepts trusted devices only. Stopping sharing disables this
automatic startup setting.

For lock screens, UAC prompts, and administrator windows, use
**Install Windows services** in the same settings page and approve the
administrator prompt. With these services installed and unattended access
enabled, trusted devices can also access the primary sign-in screen after
startup or sign-out.

After updating crDesk, reinstall the Windows services from Settings, restart
sharing, and enable unattended access again if needed. Use **Remove Windows
services** before deleting a service-enabled installation.

### Linux: keep the current session available while locked

Configure the optional helper using the [locked-session setup guide](linux/LOCK_CAPTURE.md).
Start sharing and pair a trusted viewer. While the Linux desktop is unlocked,
enable **Settings → Connect → Keep sharing when this Wayland session is locked**
and complete system authentication.

This applies to the **current sharing session**, requires one supported physical
monitor, and limits locked-screen capture to **10 FPS**. It does not enable
access after reboot or logout. Only trusted viewers can reconnect in this mode.
Remote mouse or keyboard activity can wake the screen when control is permitted.

After unlocking, crDesk attempts to restore normal desktop sharing. Whether
another permission dialog appears depends on the desktop portal's restoration
support and the validity of the previous authorization.

### Linux: access after reboot or before login

Follow the separate [boot and login-screen setup guide](linux/BOOT_ACCESS.md).
This mode requires administrator enrollment and currently targets **Debian
GNOME/GDM with Wayland**, one supported GPU, one physical monitor, and one
selected desktop user. Networking must be available before that user logs in.

Login screens and locked sessions use KMS/UHID. After login or unlock, crDesk
attempts Portal/EIS and keeps KMS/UHID available while desktop consent is pending
or the Portal agent is unavailable. A fresh login can still require consent.

This mode provides screen viewing and keyboard/mouse control. It does not
provide audio, clipboard, or file transfers, and does not cover other users,
multiple monitors, headless displays, or disk-encryption prompts before the
operating system starts.

Boot access has a separate machine-level trusted-device list. Changes in the
ordinary **Permissions** dialog do not automatically revoke that enrollment;
use the refresh or revoke commands in the setup guide. Do not run ordinary
sharing and boot access on the same listening port.

### macOS automatic sharing

Enable **Settings → Connect → Start sharing automatically after macOS login**.
crDesk reuses an eligible saved configuration or guides you through permissions,
screen selection, and trusted-device pairing. Allow the background item in
macOS Login Items settings if requested.

Keep the app at its configured location. Sharing starts after the user logs in
and accepts trusted devices only. This does not provide access to the logged-out
login window or FileVault startup unlock. Before replacing or moving the app,
disable automatic sharing, stop sharing, and quit; enable the setting again
afterward. See the [macOS guide](mac/README.md) for details.

## Updating and removing the Debian package

Install an updated deb with APT using the installation command above and the
new package filename. Upgrade scripts stop activation
sockets before services, preserve enrollment, and restore previously running
services subject to local service policy. Active connections are interrupted.
Disabled services are not enabled automatically. Locally modified service files
are preserved; replacement templates may be left as `.dpkg-dist` files.

Restart open crDesk desktop applications and Portal agents after an upgrade, or
log out and back in. An interactive lock-helper lease requires authorization
again after the helper restarts.

```sh
# Remove the application and disable its services, retaining private enrollment.
sudo apt remove crdesk

# Also remove generated system configuration and service data.
sudo apt purge crdesk
```

Removal also removes the Portal agent's automatic startup entry. Reinstalling
after removal does not reactivate unattended access; configure it explicitly.
Purge preserves personal user data, administrator service drop-ins, and the
reserved service account. Installing a deb alone never grants unattended access.

## Troubleshooting

When the host user stops screen sharing, the remote desktop displays
**The host has stopped screen sharing.** in a centered badge. The message stays
visible until you reconnect or leave the session. Both sides need a version that
supports this notification; older versions still disconnect normally.

| Symptom | What to check |
| --- | --- |
| The host is missing from the device list. | Confirm sharing is active and LAN discovery is enabled. Try **Add device** with the host's address and port. |
| The address is reachable but the desktop will not connect. | Check the connection key, local approval, firewall, and UDP connectivity. Both devices need compatible crDesk versions. The default setup does not configure a public relay server. |
| Paired-device authentication fails. | Check that the host still allows this viewer in **Permissions**. A saved pairing takes precedence over a typed connection key. To pair again, use **Forget pairing** for that host on the viewer, then reconnect using the host's current key and approve access locally. Unattended-only hosts require ordinary sharing for a new pairing. |
| A viewer is rejected after enabling unattended access. | Pair it through ordinary sharing first. For Linux boot access, also enroll or refresh the machine-level trust list. |
| The screen is visible but input does not work. | Check the host's remote-control setting, the viewer's trusted-device permissions, and operating-system input permissions. On macOS, check Accessibility. |
| Clipboard sharing does not work on Wayland. | Enable the appropriate direction and device permission. The desktop must also support and authorize the clipboard portal. |
| Linux asks for sharing permission after unlocking. | Permission restoration is controlled by the desktop portal. A new session, expired authorization, or unsupported restoration can require another approval. |
| A Linux lock-screen option cannot be enabled. | Start sharing, pair the viewer, and verify the helper setup and system authentication described in the locked-session guide. Installing the deb alone is insufficient. |
| Video is slow despite hardware encoding. | Inspect **Performance details**. Capture or conversion can limit frame rate before the encoder receives frames. GPU display and hardware encoding are separate capabilities. |
| Sharing continues after closing the host window. | This is expected. Use **Stop** for ordinary sharing; disable Linux boot access using its setup script when that mode is active. |

Use **Export Diagnostics** when reporting a connection or performance problem.
Include the crDesk versions on both devices, host/viewer operating systems,
whether Linux uses Wayland or X11, and the steps that reproduce the issue.

## Language and appearance

Settings includes light, dark, and system themes, profile names and avatars, and
interface language selection. Restart crDesk after changing its language.

Alongside English, translations are available for Japanese, Korean, Simplified
Chinese, Traditional Chinese (Hong Kong and Taiwan), Arabic, French (including
Canada), Russian, Spanish (including Latin America), and Portuguese (Brazil and
Portugal).

## Further help

- [Connection approval, pairing, clipboard, and file transfer](ACCESS_AND_FILES.md)
- [Linux locked-session sharing](linux/LOCK_CAPTURE.md)
- [Linux boot and login-screen access](linux/BOOT_ACCESS.md)
- [macOS permissions and automatic sharing](mac/README.md)

### Cloud accounts

With an account-enabled crDeskServer, open **Settings → Cloud connection →
Sign in / Manage account**. Sign in with Google in your system browser, or use a
passkey already registered to your crDesk account. After Google sign-in, you can
add a passkey for future logins. Your browser can offer a nearby phone or a local
authenticator, depending on platform support.

Confirm the device name and enter the code displayed in crDesk to register the
computer. Devices signed in to the same account can discover each other;
existing desktop approval and permission settings still apply. Use the browser
account page to remove devices or passkeys. **Sign out on this device** removes
the local cloud credential. Keep crDesk open for cloud discovery and signaling.

Server operators: see [Account setup](server/ACCOUNTS.md) for Google OAuth,
HTTPS, passkey and database configuration.
