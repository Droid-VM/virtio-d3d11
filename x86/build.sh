#!/usr/bin/env bash
# Compatibility entry; the build is maintained in win3d/build.sh.
set -euo pipefail
win3d="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec bash "$win3d/build.sh" d3d11 opengl --arch x86 "$@"
