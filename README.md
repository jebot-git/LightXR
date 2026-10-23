> This experimental branch defaults to FLTK 1.4.5. See [FLTK_EXPERIMENT.md](FLTK_EXPERIMENT.md) for build instructions and missing features.

# LightXR

A LightDM fork with a local and native OpenXR greeter, preset to launch DomeD.
The XR greeter is a curved panel 2 metres ahead, with controller pointer input and
an on-screen keyboard. A seat broker connects it to already-running, trusted
WiVRn-Pro/Monado servers while PAM authentication stays in LightDM.

See [LightXR build, installation, architecture and validation](lightxr/README.md).
Upstream's README is retained as [README.LightDM.md](README.LightDM.md), along with
its full Git history and licenses.


## Build and preview

See the [dependency list and build instructions](lightxr/README.md) first.
The `lightxr` branch contains the fork; upstream history is preserved.

```sh
export DE_XR_FLTK_PREFIX=/path/to/patched-fltk-1.4.5
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$DE_XR_FLTK_PREFIX"
cmake --build build
./build/lightxr-greeter --demo --windowed
```

The demo previews the interface without authentication. Production uses
`lightxr` (the display manager), `lightxr-greeter` (FLTK UI by default),
`lightxr-presenter` (OpenXR) and `lightxr-broker.py` (trusted runtime bridge).
The physical greeter runs in Cage on a native Wayland DRM/libinput seat;
install the `cage` runtime package. Xorg is not required.

## System installation

Install DomeD first, then follow the full guide to prepare and validate the stage:

```sh
./lightxr/stage.sh -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$DE_XR_FLTK_PREFIX"
sudo ./lightxr/install-system.sh --xr-user YOUR_XR_SERVER_USER
```

The PAM stack is selected automatically: Fedora uses authselect's
`password-auth` and `postlogin`; Debian uses the `common-*` stacks. Set
`-DLIGHTXR_PAM_STACK=fedora` or `debian` to override detection.

The daemon installs under `/usr/local/sbin`, greeter/presenter/broker under
`/usr/local/libexec/lightxr`, and configuration under `/etc/lightxr`.
Choose the trusted owner of the already-running XR server explicitly. Review the
PAM configuration for your distribution. Installation leaves the current display
manager running; selecting LightXR for next boot is a separate step described in
the guide. Controller input and the on-screen keyboard are implemented; hardware
validation details and remaining limits are recorded in that guide.

## Command manuals and related projects

Basic manuals: [lightxr.8](lightxr/man/lightxr.8), [lightxr-greeter.1](lightxr/man/lightxr-greeter.1). After installation, use `man lightxr`.

Part of the [de-xr desktop](https://github.com/topics/de-xr):
[DomeD](https://github.com/jebot-git/DomeD) provides the desktop/session,
[Ghostty XR](https://github.com/jebot-git/ghostty-xr) the terminal,
[mpvr](https://github.com/jebot-git/mpvr) video playback,
[cmuxr](https://github.com/jebot-git/cmuxr) music playback and an audio-reactive XR floor,
[fsvr](https://github.com/jebot-git/fsv-xr) file browsing,
[qimgvr](https://github.com/jebot-git/qimgvr) image viewing,
[Arc SBS](https://github.com/jebot-git/arc-sbs) depth conversion, and
[LightXR](https://github.com/jebot-git/LightXR) the optional login manager.
Each application has its own dependencies and can be used independently where
its selected backend permits.
