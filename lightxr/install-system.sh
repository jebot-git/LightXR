#!/usr/bin/env bash
# Install a prepared, tested tree. Never compile or stop a login session as root.
set -euo pipefail
umask 022
[[ $(id -u) -eq 0 ]] || { echo "Use sudo $0 --xr-user USER" >&2; exit 1; }
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
stage="$root/build/system-stage"
xr_user="${SUDO_USER:-}"
enable=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --xr-user) xr_user="$2"; shift 2;;
        --stage) stage="$2"; shift 2;;
        --enable-next-boot) enable=true; shift;;
        *) echo "Unknown option: $1" >&2; exit 2;;
    esac
done
[[ -n "$xr_user" && "$xr_user" != root ]] || { echo 'Specify the trusted XR server owner with --xr-user USER.' >&2; exit 1; }
getent passwd "$xr_user" >/dev/null
[[ -f /usr/share/wayland-sessions/dome.desktop && -x /usr/local/bin/dome-session && -x /usr/local/bin/dome-compositor ]] || { echo 'Install DomeD first.' >&2; exit 1; }
command -v cage >/dev/null || { echo 'Install Cage (Wayland kiosk compositor), its wlroots runtime and libpam-systemd first.' >&2; exit 1; }
python3 "$root/lightxr/tests/validate_stage.py" "$stage"
exec 9>/run/lock/lightxr-install.lock
flock -x 9
backup="/var/backups/lightxr/$(date +%Y%m%d-%H%M%S)"
# DomeD sessions use umask 077. Directory traversal must still be available to
# the dedicated greeter UID and to systemd's unprivileged configuration readers.
while IFS= read -r -d '' relative; do
    install -d -m 0755 -o root -g root -- "/${relative#./}"
done < <(cd "$stage" && find usr etc -type d -print0)
while IFS= read -r -d '' relative; do
    target="/${relative#./}"
    if [[ -e "$target" || -L "$target" ]]; then
        install -d -m 0755 "$backup/$(dirname -- "$relative")"
        cp -a -- "$target" "$backup/$relative"
    fi
    mode=0644
    [[ "$relative" == usr/local/sbin/* || "$relative" == usr/local/libexec/* ]] && mode=0755
    install -D -m "$mode" -o root -g root -- "$stage/$relative" "$target"
done < <(cd "$stage" && find usr etc -type f -print0)
# Only this administrator-selected runtime owner may receive greeter frames/input.
python3 - "$xr_user" <<'PY'
import json,os,sys
from pathlib import Path
p=Path('/etc/lightxr/broker.json');config=json.loads(p.read_text());config['allow_users']=[sys.argv[1]]
tmp=p.with_suffix('.new');tmp.write_text(json.dumps(config,indent=2)+'\n');tmp.chmod(0o644);os.replace(tmp,p)
PY
systemd-sysusers /usr/local/lib/sysusers.d/lightxr.conf
systemd-tmpfiles --create /usr/local/lib/tmpfiles.d/lightxr.conf
# Ubuntu ships this helper in /usr/share; the installed DomeD launcher uses
# the upstream libexec path. Preserve the PAM wallet handoff outside Plasma.
if [[ ! -e /usr/libexec/pam_kwallet_init && -x /usr/share/libpam-kwallet-common/pam_kwallet_init ]]; then
    install -d -m 0755 /usr/libexec
    ln -s /usr/share/libpam-kwallet-common/pam_kwallet_init /usr/libexec/pam_kwallet_init
fi
systemctl reload dbus.service
systemctl daemon-reload
if $enable; then
    systemctl enable lightxr.service --force
    echo 'LightXR selected for the next boot. Current sessions remain running.'
else
    echo 'Installed LightXR. To select it at next boot: systemctl enable lightxr.service --force'
fi
echo "Previous files, if any: $backup"
