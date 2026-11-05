# macOS application and permissions

crDesk uses one executable for two process roles. The normal process owns the
window; `crDesk --host-process` owns authorized capture, audio, input and the
connection. Closing the main window hides the app without exiting or stopping
sharing, including when sharing is off. The Host's menu-bar Open
command activates an existing regular UI process or starts one without the Host
switch. Both roles use the same bundle identifier and executable signature.

The Host's menu-bar entry loads `crDesk.icns` directly through AppKit, without
requiring the UI's Chromium ResourceBundle. A system desktop symbol is used if
the icon is unavailable (for example, when running a loose executable).
Left-click hides a visible crDesk UI or shows it again; if the UI has exited,
it starts a new UI attached to
the existing Host. Hiding the UI keeps sharing active. Right-click (or
Control-click) opens a menu containing Open, Pause, Allow control and Stop,
followed by a separator and Quit.
Revoke access, Change key, Clipboard and Switch screen are omitted from this
macOS menu. The remaining labels use the existing UI translations.

The UI owns an idle menu-bar icon while sharing is off; the Host owns the icon
while sharing is active. Stopping sharing returns to the idle icon, with Pause,
Allow control and Stop disabled and Open still available. Left-click also
toggles the UI in this state. If the UI has already exited, Stop from the Host
menu starts it with `--start-hidden` to retain the idle entry without presenting
its window. The stopped Host still exits and releases capture resources.
Quit is available from both idle and sharing menus. It disables automatic
startup, stops sharing and exits the UI and Host for this app bundle. Previous
configuration is retained for the next explicit enable action. Closing Settings
or other dialogs still closes those dialogs normally.
For the idle icon, clicking brings a hidden, minimized or inactive main window
to the foreground; clicking again while the main window is active hides the
application. Restoring a hidden launch explicitly activates the Chromium Widget
so it does not retain the ordering used by `ShowInactive()`.

Screen sharing requires macOS 14 or later, matching the Chromium ScreenCaptureKit
desktop backend. The package's minimum system version must also satisfy the
deployment target of every included binary.

## Permissions

- Screen recording is requested when selecting screens. A denied request is
  reported in the UI; allow crDesk in System Settings, then retry sharing. macOS
  may require quitting and reopening crDesk after a permission change.
- Microphone access is requested only when enabling the microphone. Completion
  runs asynchronously; a stopped connection or changed device invalidates the
  pending request. Merely opening Settings does not request capture access.
- Accessibility controls remote keyboard and mouse injection. Without it,
  screen sharing can continue in view-only mode. The input implementation checks
  authorization again while sharing.
- System audio uses Chromium's Core Audio tap or ScreenCaptureKit audio backend.
  Audio taps have no public permission-preflight API: Settings shows that access
  is checked when audio sharing starts, rather than incorrectly claiming access
  is granted. A permission failure from the stream is reported separately from
  an unavailable device. Audio failure does not close the desktop connection.

Settings / Audio shows the four access states and links to the corresponding
system privacy settings. The page refreshes these states while it is visible.
Info.plist includes microphone, system audio and local-network purpose strings;
localized descriptions cover English and all 13 supported translation locales.
The development executable embeds the same English metadata, but permission
identity for a loose executable can depend on the launching terminal. Use an
application bundle at a stable path for permission verification.

The direct-download signature uses Apple's
[audio-input entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.device.audio-input).
This is an unsandboxed remote-control app. These signing inputs do not claim
Mac App Store sandbox compatibility or provide unattended login-screen access.

## Start sharing after login

In Settings / Connect, enable **Start sharing automatically after macOS login**.
You do not need to start sharing first. If a previous configuration, trusted
device and required permissions are available, the switch registers startup
directly and the background agent restores sharing. Otherwise Settings closes
and setup continues in the main window: grant Screen Recording and, for remote
control, Accessibility, choose a display, then connect and remember a trusted
device using the existing pairing flow. Once these steps finish, startup is
enabled automatically without clicking the switch again. If sharing is already
active and ready, the switch saves that session directly.

During setup the main window shows instructions. Stop sharing, dismiss the
initial screen selector, or reopen Settings and turn the pending switch off to
cancel. Setup also expires after five minutes, or if the UI exits; it does not
silently register startup later. Canceling the switch leaves an already active
manual sharing session running; Stop ends sharing as well. A failed or timed-out
setup does not mark the switch enabled. Use a signed app bundle produced by
`package_macos.py` or `run_macos.py` at a stable location. A loose build executable
does not contain the login service registration plist.

The setting registers a per-user Aqua LaunchAgent using `SMAppService`. If macOS
requires approval, use **Open Login Items Settings** and allow crDesk to run in
the background. The settings page distinguishes saved authorization from the
system's background-service approval. Registration is performed only in response
to enabling the setting, never merely by opening the app.

The agent runs the same `Contents/MacOS/crDesk` executable with `--unattended` as
a monitor. It starts `--host-process --unattended` only when no Host holds the
existing lock. No separate crDeskHost binary or root daemon is installed. The
monitor checks for a missing Host and backs off failed starts from 10 to 60
seconds; a stable Host resets the delay to five seconds. The UI is not opened at
login. Reopening the UI attaches to a running sharing Host through its existing
authenticated local control endpoint.

The private `~/.crdesk/unattended-mac.json` saves the display UUID, listening
port, frame rate, control/clipboard permissions, pause state, discovery,
system-audio preference, audio device IDs and localized Host menu labels. It
contains no macOS password and no remote device credentials; the existing trust
store remains separate. Startup accepts trusted devices only. Microphone capture
is not automatically enabled. Configuration writes are atomic and serialized
with a private file lock; reads reject symlinks, foreign owners and files with
group/other access.

`~/.crdesk/unattended-mac-previous.json` retains configuration for the next
explicit enable action, including after Stop or turning the switch off. It has
the same private-file checks and bundle-path binding. It grants no startup
authorization: the monitor and Host read only `unattended-mac.json`. Removing
that active file still disables automatic sharing.

If the saved display or required permissions are unavailable, the Host waits
and retries without selecting a different display or opening a privacy prompt.
Capture failures retain the startup authorization so the Host can retry.
Disabling the background item in macOS prevents automatic sharing while it is
disabled. Closing the app window keeps sharing and startup enabled. **Stop** in
the UI or Host menu disables automatic startup as well as stopping sharing.
Unchecking automatic startup leaves an active manually managed sharing session
running. The setting can also be disabled from Settings when no Host is sharing.

Keep the app at the same path. Before moving, replacing or uninstalling it,
disable automatic sharing and stop sharing, then quit crDesk. This also stops
the monitor so the development launcher can safely replace the bundle. After
an update, reopen the app and enable the setting again. A moved bundle must be
authorized again; the saved configuration is bound to its original bundle path.

This feature starts **after the user logs in**. It does not implement logged-out
login-window access, FileVault startup unlock, or waking a sleeping Mac over the
network. Locking an existing logged-in session is a separate supported test path.

Manual acceptance after rebuilding and repackaging:

1. With sharing stopped and no previous configuration, enable the setting.
   Complete permission, display selection and trusted-device pairing steps from
   the main window. Confirm automatic startup becomes enabled without another
   toggle, and approve the background item if requested. Close the Mac window
   and confirm sharing continues.
2. Log out and log back in locally. Without opening crDesk, reconnect from the
   trusted client. Confirm the saved display and control/clipboard settings and
   that an unpaired client cannot connect.
3. Open crDesk and verify it attaches to the existing Host. Lock the Mac,
   disconnect the client, reconnect and verify remote input.
4. With startup enabled, terminate the Host during a controlled test and verify
   the monitor restarts it. Test missing/reconnected displays and revoked/restored
   screen permission; another display must never be substituted automatically.
5. Test Stop from both the UI and menu, disabling from Settings without a sharing
   Host, and disabling the background item in macOS. Verify no automatic capture
   resumes after the relevant authorization is removed.
6. With sharing stopped, re-enable the switch and verify the previous display
   and settings are restored without manually starting sharing. During fresh
   setup, test canceling the screen selector, turning the pending switch off,
   closing the UI and the five-minute timeout. No canceled setup may register
   startup later. Reopen Settings after each case and check the switch state.
7. With sharing both on and off, repeatedly close the main window and restore it
   from the menu-bar icon. Verify Settings still closes normally. Use Quit from
   both idle and sharing menus (including a login-started Host with no UI), and
   verify the UI, Host and login monitor exit without being restarted.

## Run an existing development build

Stop sharing and quit crDesk, including its background Host, then run from the
Chromium source directory:

```sh
python3 crDesk/tools/run_macos.py out/arm64
```

The launcher packages existing outputs into `out/arm64/crDesk.app` and opens
that exact bundle through Launch Services. It prints
the log paths and streams application logs to the terminal. It never compiles,
installs, or terminates an existing crDesk process. Closing the UI can leave a
sharing Host running; stop sharing before starting the launcher again. Ctrl+C
stops log monitoring without stopping crDesk.

For permission testing, use an Apple Development or Developer ID Application
certificate from your keychain. Keep the bundle identifier, path, signing team
and certificate type consistent across runs:

```sh
security find-identity -v -p codesigning
python3 crDesk/tools/run_macos.py \
  --identity 'Apple Development: Your Name (TEAMID)' --team-id TEAMID out/arm64

# Alternatively, reuse a signing identity in this shell.
export CRDESK_MAC_SIGNING_IDENTITY='Developer ID Application: Your Name (TEAMID)'
python3 crDesk/tools/run_macos.py --team-id TEAMID out/arm64
```

`--identity` accepts a certificate name or hash and overrides the environment
variable. With neither set, the launcher uses ad-hoc signing and prints a notice
that permissions may not survive a rebuild. Certificate signing validates the
certificate type and Team ID, and never falls back to ad-hoc signing on failure.
Development signing uses no timestamp or Hardened Runtime and is for local
testing. Use the distribution packager below for release signing.

Subsequent runs replace only a bundle marked `CrDeskDevelopmentBundle` with the
same bundle identifier, after the replacement has passed signature verification.
An existing release app or unmarked bundle is not overwritten. The launcher
preserves the original build outputs.

```sh
# Package without opening the application.
python3 crDesk/tools/run_macos.py --prepare-only out/arm64

# Choose a fixed output path and pass arguments to the application.
python3 crDesk/tools/run_macos.py --output /tmp/crDesk-dev.app out/arm64 -- --lang=ja
```

Launcher options must precede the build directory. Grant screen recording and
Accessibility access to the generated app. Keep its path stable for development;
an ad-hoc signature does not guarantee permission grants survive a rebuild.
After changing from ad-hoc to certificate signing, quit both UI and Host and
grant access to the current app in System Settings if required. The launcher
does not reset or grant system permissions.

Both UI and Host log `crDesk: macOS permissions` at startup. These entries include
the process role and PID, bundle identifier and paths, usage-description presence,
and screen, microphone, Accessibility and system-audio permission states.
Further entries accompany screen/microphone requests, audio failures and changes
observed in the settings page. Compare the UI and Host entries when the settings
page shows access but sharing fails. `system_audio=checked-on-use` means the
permission is checked when opening the audio stream, not that access is granted;
an audio open failure reports `permission-denied` or `device-unavailable`.

## Diagnose lock-screen input

Build the updated sources on both machines before testing. Stop sharing and quit
both the Mac UI and its background Host before using the launcher; attaching to
an existing Host does not enable diagnostics in that process. Keep the current
signing identity and bundle path:

```sh
python3 crDesk/tools/run_macos.py \
  --identity 'Developer ID Application: RUXU WU (6JYK2B5HZ3)' \
  out/arm64 -- --input-diagnostics
```

On Linux, start the updated client with `--input-diagnostics`, redirecting stderr
to a file (replace the executable path with the actual build output):

```sh
/path/to/crDesk --input-diagnostics 2> /tmp/crdesk-input-linux.log
```

Connect while the Mac is unlocked and verify keyboard and mouse control. Lock
the Mac locally, try remote mouse movement, a click and a non-sensitive key,
then unlock locally and repeat. Spend at least ten seconds in each phase. Record
the approximate lock/unlock times and keep the Mac launcher's printed log file
and the Linux log. Passwords are not needed for this diagnostic test.

The opt-in `crDesk input diagnostics` entries contain only states and aggregate
counts, never key codes, text, coordinates, epochs, credentials or message bodies.
The switch is forwarded to a newly launched Host. Counts reset after each log,
normally every five seconds, and also flush at state transitions and destruction.
Zero-count entries show that the diagnostic timer is still running. IDs identify
individual diagnostic objects within a process; use the log's PID as well.

- `role=viewer`: `generated` counts calls to the input callback; `submitted`
  counts messages handed to the viewer transport, not confirmed network delivery.
  Pointer coalescing can reduce the latter. `enabled`, `active`, `frame`,
  `control`, `pending`, `paused` and `has_epoch` explain the client input gate.
- `role=host`: `received` counts incoming control-channel messages, including
  non-input requests such as statistics. `handled` counts messages accepted by
  the desktop input parser, not confirmed OS delivery. `inactive`, `malformed`,
  `epoch_mismatch`, `control_blocked`, `stale_pointer` and `handle_failed` identify
  early exits. `lock_blocked` applies only to the Linux Host backend.
- `role=mac-native`: `post_called` counts calls to `CGEventPost`, including key
  and button releases. The API has no delivery acknowledgment. `create_failed`,
  `unmapped_key` and `permission_blocked` identify native early exits.
  Permission/session states are sampled once per second and logged on changes;
  `post_access` uses the non-prompting `CGPreflightPostEventAccess` check.
  `lock_notification` is `-1` until a lock/unlock notification is observed, then
  `1` for locked or `0` for unlocked. `on_console` and `login_done` are session
  properties, with `-1` meaning unavailable; neither is a lock-state substitute.

If client submission continues but Host reception stops, investigate the data
channel. If Host reception continues but handling stops, inspect the rejection
counters and control state. If handling and native posting continue while the
lock screen ignores input, investigate macOS session/event delivery. This flag
does not change permissions, event injection, or lock-screen support.

## Package an existing build

The packaging tool never compiles, installs, uploads, or replaces an existing
output directory. On a Mac with command-line developer tools installed:

```sh
python3 crDesk/tools/package_macos.py --build-dir out/mac.arm64 \
  --output /tmp/crDesk-development.app --development
```

Add `--identity` and `--team-id` to `--development` to use a certificate for local
permission testing. Without `--development`, `--identity` requires a Developer
ID Application certificate and enables Hardened Runtime and a secure timestamp.

For a Developer ID application:

```sh
python3 crDesk/tools/package_macos.py --build-dir out/mac.arm64 \
  --output /tmp/crDesk-release.app --build-number 1 \
  --identity 'Developer ID Application: Your Name (TEAMID)' \
  --team-id TEAMID \
  --icon /path/to/crDesk.icns
```

The icon argument accepts ICNS or ICO files and defaults to the project's
`resources/win/crdesk_app_icon.ico`, converted to ICNS during packaging.
`--bundle-id` defaults to `com.libcr.crdesk`; `--team-id` defaults to
`6JYK2B5HZ3` and must match the Developer ID certificate. `--strip` strips only
staged executables and libraries before signing. `--check-only` validates build
resources, localizations, binary architectures and deployment targets without
creating the output or requiring a signing mode.

The tool packages required paks and ICU data,
available snapshots and private dylib dependencies, including dynamic ANGLE when
enabled. It verifies architecture and deployment targets, relocates private
dependencies, signs nested libraries before the app, and verifies the signature.
`--unsigned` stages a layout for inspection only. Notarization and stapling are
separate release steps; a signing identity must be supplied by the developer.

Only `Contents/MacOS/crDesk` is copied. No `crDeskHost` binary is packaged. Keep
the bundle intact so UI and Host can load the same resources and libraries.

## Capture behavior and manual verification

Screen changes create a fresh capturer so ScreenCaptureKit cannot return the
previous screen's cached surface. Pausing invalidates queued delivery/conversion
callbacks. Removed displays end capture rather than sharing another display.
Native unchanged regions skip GPU conversion and encoding, with a one-second
refresh plus immediate refresh requests for new connections and keyframes.
The healthy-capture callback still updates input availability and the watchdog.

Verify on macOS hardware after building:

1. Start sharing, close the main window, then use the menu-bar Open command.
   The window should reopen without interrupting the remote connection.
2. Switch repeatedly between two equally sized displays with distinct content,
   also testing mixed Retina scales, pause/resume and display removal. No old
   screen frame should be delivered with the new screen's coordinates.
3. With a connected client and no desktop changes, check `SCK capture damage
   stats`: unchanged frames should increase and delivered FPS should approach
   one. Motion should resume promptly; keyframe requests and reconnects must
   still produce an image. Compare CPU usage with identical audio/preview settings.
4. Grant and deny screen, microphone, Accessibility and system-audio permissions.
   Check the status text, retries and links; returning from System Settings should
   refresh the displayed state. Test disconnect while a microphone prompt is open.
5. Close the UI and stop capture using the system sharing control. Confirm the
   client disconnects cleanly and the background Host releases capture resources.
6. Launch the staged app from Finder on a machine without the build directory.
   Verify resource loading, UI-to-Host startup, microphone capture, system-audio
   playback and menu-bar reopening with both Apple Silicon and Intel builds.
