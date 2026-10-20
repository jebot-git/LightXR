#!/usr/bin/env bash
# Build and stage locally; no root or display-manager changes.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="${LIGHTXR_BUILD_DIR:-$root/build}"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr/local "$@"
cmake --build "$build" --parallel "${LIGHTXR_JOBS:-4}"
ctest --test-dir "$build" --output-on-failure
DESTDIR="$build/system-stage" cmake --install "$build"
python3 "$root/lightxr/tests/validate_stage.py" "$build/system-stage"
echo "LightXR staged at $build/system-stage. The active login manager was not changed."
