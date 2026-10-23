# LightXR

LightXR is a separate LightDM fork with an FLTK greeter (Qt6 is an explicit
build option), a root-owned seat broker and
an unprivileged native OpenXR presenter. DomeD is the preset user session. The
existing LightDM/PAM authentication and logind session machinery remain in use;
the XR presenter never authenticates a user or receives the PAM connection.
Upstream history is retained. `UPSTREAM.json` pins the LightDM base and the latest
WiVRn-Pro revision inspected during implementation.

## Build and preview

Ubuntu/Debian development dependencies:

```sh
sudo apt install build-essential cmake ninja-build pkg-config libfontconfig1-dev libfreetype-dev \
  libglib2.0-dev libxcb1-dev libxdmcp-dev libx11-dev libxklavier-dev \
  libpam0g-dev libgcrypt20-dev libopenxr-dev libegl-dev libgl-dev \
  desktop-file-utils python3
export DE_XR_FLTK_PREFIX=/path/to/patched-fltk-1.4.5
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$DE_XR_FLTK_PREFIX"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
# Optional installed-system proof while a headset is connected and no greeter is active:
sudo python3 lightxr/tests/test_installed_xr.py --xr-user YOUR_XR_SERVER_USER
# This brief authentication-disabled clock preview checks the dedicated account,
# production broker, capability-free presenter and visible headset frames.
./build/lightxr-greeter --demo --windowed
```

The default build requires the shared patched FLTK **1.4.5** prefix; an older
system FLTK is not interchangeable. Keep that runtime available to the installed
greeter. The build links Fontconfig and FreeType. Qt is not required by the FLTK
greeter or its native presenter. To opt into the retained Qt6 implementation,
install Qt6 Widgets/Network/Test development files and configure a separate tree:

```sh
cmake -S . -B build-qt6 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDE_XR_TOOLKIT=Qt6
cmake --build build-qt6 --parallel 4
```

`--demo` renders a preview with authentication disabled. Production authentication
is available only through LightDM's private greeter channel. Keyboard/mouse work
in the local greeter; the same interface, including its on-screen keyboard, is
rendered in XR. The OpenXR background is embedded and the login card is opaque to
keep account fields, PAM prompts and status messages readable.

## Install and select at next boot

LightXR reads DomeD's existing `/usr/share/wayland-sessions/dome.desktop`
(or its standard `/usr/local/share/wayland-sessions` override). It does not install
or shadow a second copy. Updates installed by DomeD's own installer are therefore
picked up by the manager. The desktop file's Wayland session type, LightDM's
`XDG_SEAT_PATH`/`XDG_SESSION_PATH` switch-user interface, logind seat/VT metadata,
and `/dev/ttyN` PAM terminal are preserved. Stale greeter X11/Wayland display
variables are removed before the native user session starts.

The installed launcher remains responsible for its private D-Bus session, logind
libseat backend, XR startup, Ghostty XR terminal mode, IME setup, KWallet handoff,
portal/toolkit services and logout supervisor. LightXR supplies PAM credentials
and the same locale/KWallet hooks as the installed SDDM policy. It does not replace
DomeD's launcher, compositor, session services, user settings or environment choices.

The installer places its system D-Bus policy in `/etc/dbus-1/system.d`, which is
loaded even when binaries use `/usr/local`. It sets public directory permissions
explicitly so DomeD's restrictive session umask cannot prevent the greeter account
from starting. On Ubuntu it also exposes the distribution's PAM KWallet helper at
the libexec path used by older installed DomeD launchers.

The runtime needs `cage` (and its wlroots dependencies) and `libpam-systemd`
for the greeter's physical seat, and DomeD installed at `/usr/local/bin/dome-session`.
The greeter desktop entry uses `X-LightDM-Session-Type=wayland` and launches
`lightxr-wayland`: an unprivileged Cage kiosk with native DRM/libinput and
logind seat access. Its FLTK client uses Wayland exclusively; inherited display
variables are cleared and no Xorg or Xwayland server is required. Cage manages
the window size and input focus and allows normal VT switching (`-s`). The
authenticated DomeD desktop starts separately on its own Wayland seat session.
The PAM policy is selected automatically: Fedora uses authselect's `password-auth`
and `postlogin`, while Debian/Ubuntu uses the `common-*` stack, including configured
providers and MFA. Set `-DLIGHTXR_PAM_STACK=fedora` or `debian` to override detection.
Review the policy before installing on another distribution. Guest login and
automatic login are disabled.

```sh
./lightxr/stage.sh -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$DE_XR_FLTK_PREFIX"
sudo ./lightxr/install-system.sh --xr-user YOUR_XR_SERVER_USER --xr-service wivrn-pro.service
# After reviewing the install and when ready for the next boot:
sudo systemctl enable lightxr.service --force
```

The installer does not restart or stop the current display manager. Its optional
`--enable-next-boot` flag performs the final enable command above. Existing files
are backed up under `/var/backups/lightxr/`. To roll back before reboot, re-enable
the previous display-manager service with `--force` (for this host, `sddm.service`).
Keep a working TTY available for the first real login test. Activation is always
an explicit administrator action.

The installed daemon is `lightxr`; its session worker re-executes `/proc/self/exe`
so another `lightdm` on PATH cannot be substituted. Its PAM services, pid file,
cache, state, log and runtime directories are distinct. LightXR also uses its own
configuration namespace; another LightDM installation’s vendor/autologin settings
are not inherited. LightDM's D-Bus and greeter
protocols remain compatible. System display-manager D-Bus ownership still permits
only one active display manager per seat setup.

## Already-running XR servers

`/etc/lightxr/broker.json` lists allowed server owners and system-installed runtime
manifests/socket names. The installer presets the administrator-selected
`--xr-user` as a trusted server owner. It detects listening sockets under that
owner's `/run/user/UID` without opening a connection or starting an XR server.
The presenter starts only while a greeter is visible. An already-connected headset
receives it immediately after attachment; if WiVRn starts its compositor on a
headset connection, detection/retry attaches within about one second. Runtime loss
and reconnects are handled by re-creating the presenter. Every runtime needs
`XR_EXTX_overlay`, `XR_MNDX_egl_enable`, OpenGL and cylinder-layer support.

The panel is a view-relative `XR_KHR_composition_layer_cylinder` with a 2 m radius,
0.8 radian horizontal arc and 1280×800 surface. Its central point is exactly 2 m
in front of the viewer. Controller aim rays intersect that same cylinder; trigger
presses become greeter mouse events. Simple, Touch, Index and Vive controller
profiles are bound. The on-screen keyboard works with pointing/clicking. Physical
keyboard/mouse events go through the active greeter seat; WiVRn-Pro's uinput devices
use that same seat when registered there. A server running for an inactive seat
must have its input devices assigned to the login seat for physical remote input.

No WiVRn-Pro changes are needed for this interface. Latest `origin/main`
`3b7a6c827f94d13599d0554d89cd69bebd321bac` was fetched and its compositor-start and
headset reconnect paths inspected. The local WiVRn-Pro checkout was preserved.
A pre-login XR server must already be running independently of the session being
logged into (for example a system service or intentionally lingering user service).
LightXR does not log a user in or start WiVRn just to display its greeter.

### Boot availability and screen locking

`--xr-service wivrn-pro.service` enables the trusted owner's existing user service
and lingering, and orders the broker after that user's manager. It does not
restart the headset server or the display manager. Detection accepts the listening
compositor socket even when connected client entries appear first in `/proc/net/unix`.
A headset connecting after the greeter appears is picked up by the retry loop.

The FLTK greeter honors LightDM's `lock-screen` hint. Its cover displays UNIX
seconds using the bundled 24px HeadUpDaisy bitmap at exactly 2x. A click or key
reveals authentication and is consumed; it never becomes an account/password
character. The selected account remains fixed and LightDM/PAM alone can unlock
and return to the existing session. `lightxr-greeter --lock-preview --windowed`
previews the clock with authentication disabled.

DomeD's native session now exposes `org.freedesktop.ScreenSaver` at `/ScreenSaver`.
Its Lock command requests LightXR's session lock and shields desktop and XR
content/input while locked, including when the original VT is revisited. Only
logind's trusted system-bus Unlock signal releases that shield after PAM.
`SetActive(false)` cannot bypass authentication. Install the matching DomeD
compositor and renderer before using this lock integration. No user password is
handled by the compositor, presenter or runtime broker.

Greeter frames are rendered into a fixed 1280x800 OpenGL target before transport;
this preserves bitmap cells independently of the physical monitor's size/DPI
and avoids black captures from trying to print a GL window into a CPU surface. The installed greeter retains the configured FLTK runtime search path;
stage validation clears `LD_LIBRARY_PATH` when checking its dependencies, matching
the dedicated account's clean boot environment.
The native presenter uploads that frame on the GPU at the same 2m cylinder.

### Privilege boundary

The broker accepts greeter connections only from the dedicated `lightxr` UID,
verified with `SO_PEERCRED`; its public socket is root-owned and restricted to the
`lightxr` group. Only root-configured runtime owners receive frames. The broker's bounded capability set explicitly retains only socket ownership,
read/traversal, and UID/GID switching privileges; ambient UID/GID capabilities
allow that drop under the service's clean hardened environment. The presenter
runs as that runtime owner with its existing groups, no capabilities and a clean
environment. It uses a system-selected runtime manifest and one inherited private socket. It receives
pixels and returns bounded pointer/key events. It cannot choose a session, submit
a PAM verdict, issue a command, or access LightDM's authentication descriptors.
Input and frame sizes, coordinates, event types and input rates are validated.
A readiness handshake waits for OpenXR initialization before sending frames;
incremental decoding accepts startup bursts. Backpressure drops old frames
instead of accumulating them. Login or greeter exit
closes all presenters and clears retained pixels. Treat configured server owners
as trusted input/display endpoints, as with any remote login display.

## Validation

```sh
ctest --test-dir build --output-on-failure
# A running Monado test compositor is needed for this additional native test:
python3 lightxr/tests/test_xr.py --build build
# Or an already-connected WiVRn-Pro compositor:
python3 lightxr/tests/test_xr.py --build build \
  --manifest /usr/local/share/openxr/1/openxr_wivrn_pro.json \
  --runtime-socket wivrn/comp_ipc
```

The CMake suite covers broker/session policy, channel framing and cylinder
geometry. The FLTK `fltk-wayland-protocol` test runs the real greeter under Cage's
headless backend with X11 unavailable, checking the daemon protocol, secret
response, retry, XR control keys/modifiers and frame export. It skips if Cage is
not installed. `pam-session-handoff` runs the real session worker unprivileged
with a test-only PAM probe that rejects opening a session; it checks final
PAM_TTY/PAM_XDISPLAY metadata after a VT change. No actual login or credentials
are used by either fixture. The separate `greeter-protocol` Qt Test target is
only built with `DE_XR_TOOLKIT=Qt6`.

The existing protocol and integration tests cover the real liblightdm wire
protocol, authentication gating,
multiple PAM prompts, masked secret entry, cancellation, failed authentication,
DomeD handoff, actual installed launcher/supervisor environment, Linux VT/PAM terminal, local keys, XR mouse/key input, broker peer authorization, frame
bounds, malformed packets, input restrictions, helper reaping, and curved-panel
hit geometry. `test_xr.py` runs the actual greeter→broker→native presenter path
and records submitted cylinder frames. On this development host it passed with
Monado's simulated headset. On 2026-10-23, the FLTK/Cage DRM greeter accepted
physical keyboard input and completed a privileged Fedora/Nobara PAM login into
DomeD. Logind registered an active Wayland user session on its new VT, and the
DomeD compositor, XR process and tray started successfully. This does not verify
a real headset's greeter appearance or controller focus; those still need
separate device validation.

## Licensing

LightDM's GPL/LGPL notices and full history are retained. New LightXR daemon,
greeter, broker and presenter work is GPL-3.0-or-later. `src/graphics.cpp` and
`src/graphics.hpp` adapt Ghostty XR's MIT-licensed windowless EGL context helper;
its notice is retained in `COPYING.graphics`. The background's provenance and
wordmark information are in `assets/README.md`.

### Bitmap interface font

The local and XR greeter use the bundled GNU Unifont 17.0.04 bitmap face at its
native 16 px strike, including account fields, prompts, buttons and the on-screen
keyboard. The FLTK greeter loads the installed font from
`/usr/local/share/lightxr/fonts/unifont-17.0.04.pcf` (or the source-tree fallback
when developing). Keep that file with the installation. The Qt6 alternative
embeds the font as a resource. Neither requires a user's home font collection.
Text is not resized or
synthetically emboldened. Font licensing and provenance are installed in the
LightXR documentation's `fonts` directory.
