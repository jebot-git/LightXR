# LightXR

LightXR is a separate LightDM fork with a Qt greeter, a root-owned seat broker and
an unprivileged native OpenXR presenter. DomeD is the preset user session. The
existing LightDM/PAM authentication and logind session machinery remain in use;
the XR presenter never authenticates a user or receives the PAM connection.
Upstream history is retained. `UPSTREAM.json` pins the LightDM base and the latest
WiVRn-Pro revision inspected during implementation.

## Build and preview

Ubuntu/Debian development dependencies:

```sh
sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev \
  libglib2.0-dev libxcb1-dev libxdmcp-dev libx11-dev libxklavier-dev \
  libpam0g-dev libgcrypt20-dev libopenxr-dev libegl-dev libgl-dev \
  desktop-file-utils python3
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/lightxr-greeter --demo --windowed
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

The runtime needs `xserver-xorg-core`, `xserver-xorg-input-libinput` and
`libpam-systemd` for the greeter's physical seat and DomeD installed
at `/usr/local/bin/dome-session`. DomeD itself runs as a Wayland user session.
The PAM policy supplied here targets Debian/Ubuntu's `common-*` stack, including
configured providers and MFA. Adapt it to the distribution before installation
elsewhere. Guest login and automatic login are disabled.

```sh
./lightxr/stage.sh
sudo ./lightxr/install-system.sh --xr-user YOUR_XR_SERVER_USER
# After reviewing the install and when ready for the next boot:
sudo systemctl enable lightxr.service --force
```

The installer does not restart or stop the current display manager. Its optional
`--enable-next-boot` flag performs the final enable command above. Existing files
are backed up under `/var/backups/lightxr/`. To roll back before reboot, re-enable
the previous display-manager service with `--force` (for this host, `sddm.service`).
Keep a working TTY available for the first real login test. The first deployment
has not been activated automatically.

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

### Privilege boundary

The broker accepts greeter connections only from the dedicated `lightxr` UID,
verified with `SO_PEERCRED`; its public socket is root-owned and restricted to the
`lightxr` group. Only root-configured runtime owners receive frames. The presenter
runs as that runtime owner with its existing groups, a clean environment, a
system-selected runtime manifest, and one inherited private socket. It receives
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

The tests cover the real liblightdm wire protocol, authentication gating,
multiple PAM prompts, masked secret entry, cancellation, failed authentication,
DomeD handoff, actual installed launcher/supervisor environment, Linux VT/PAM terminal, local keys, XR mouse/key input, broker peer authorization, frame
bounds, malformed packets, input restrictions, helper reaping, and curved-panel
hit geometry. `test_xr.py` runs the actual greeter→broker→native presenter path
and records submitted cylinder frames. On this development host it passed with
Monado's simulated headset. A real headset's appearance, controller focus and a
real privileged console/PAM login still need deployment validation; simulation
is not evidence of those device- and seat-specific behaviors.

## Licensing

LightDM's GPL/LGPL notices and full history are retained. New LightXR daemon,
greeter, broker and presenter work is GPL-3.0-or-later. `src/graphics.cpp` and
`src/graphics.hpp` adapt Ghostty XR's MIT-licensed windowless EGL context helper;
its notice is retained in `COPYING.graphics`. The background's provenance and
wordmark information are in `assets/README.md`.
