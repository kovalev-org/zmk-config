#!/usr/bin/env bash
#
# Initialise / refresh the west workspace inside the ZMK build container.
#
#   tools/west-init.sh          # init if needed, then update + zephyr-export
#   tools/west-init.sh update   # just re-run `west update` (after west.yml edits)
#
# Populates .west/, zmk/, zephyr/ and modules/ in the repo. All of these are
# gitignored. Run this once after a fresh clone, and again whenever
# config/west.yml revisions change.

set -euo pipefail

IMAGE="${KEYBALL39_IMAGE:-docker.io/zmkfirmware/zmk-build-arm:stable}"
WS=/workspaces/zmk-config

in_container() { [ -n "${container:-}" ] || [ -f /.dockerenv ]; }

if ! in_container; then
    repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

    engine="${KEYBALL39_ENGINE:-}"
    if [ -z "$engine" ]; then
        if command -v podman >/dev/null 2>&1; then engine=podman
        elif command -v docker >/dev/null 2>&1; then engine=docker
        else echo "error: neither podman nor docker found on PATH" >&2; exit 1
        fi
    fi

    tty_args=()
    [ -t 1 ] && tty_args=(-t)

    exec "$engine" run --rm "${tty_args[@]}" \
        -v "$repo:$WS" -w "$WS" \
        "$IMAGE" "$WS/tools/west-init.sh" "$@"
fi

mode="${1:-init}"

if [ "$mode" = "init" ] && [ ! -d "$WS/.west" ]; then
    echo "--- west init -l config"
    west init -l config
fi

echo "--- west update (blobless fetch)"
west update --fetch-opt=--filter=blob:none

echo "--- west zephyr-export"
west zephyr-export

echo
echo "workspace ready:"
for d in zmk zephyr modules; do
    printf '  %-10s %s\n' "$d" "$([ -d "$WS/$d" ] && echo present || echo MISSING)"
done
