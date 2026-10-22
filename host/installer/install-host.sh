#!/usr/bin/env bash
set -euo pipefail
installer_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ $# -eq 0 ]]; then set -- install; fi
exec python3 "$installer_dir/install_host.py" "$@"
