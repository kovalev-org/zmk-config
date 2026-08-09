#!/usr/bin/env bash
#
# Build Keyball39 firmware inside the ZMK build container.
#
# Nothing is installed on the host: the ARM toolchain, CMake, Ninja, west and
# the Zephyr SDK all live in the `zmkfirmware/zmk-build-arm` image. The repo is
# bind-mounted, so `.build/` and `firmware/` are ordinary directories you can
# read from the host, and ccache lives in `.ccache/` (gitignored).
#
# This script re-executes itself inside the container, so there is exactly one
# copy of the build logic.
#
#   tools/build.sh              # build every target in build.yaml
#   tools/build.sh right        # build targets matching "right"
#   tools/build.sh left right   # several filters
#   tools/build.sh -p           # pristine (wipe build dir first)
#   tools/build.sh -- -DFOO=1   # pass extra args through to `west build`
#
# Env overrides:
#   KEYBALL39_IMAGE   container image (default zmkfirmware/zmk-build-arm:stable)
#   KEYBALL39_ENGINE  podman | docker (default: podman if present)

set -euo pipefail

IMAGE="${KEYBALL39_IMAGE:-docker.io/zmkfirmware/zmk-build-arm:stable}"
WS=/workspaces/zmk-config

in_container() { [ -n "${container:-}" ] || [ -f /.dockerenv ]; }

# ---------------------------------------------------------------------------
# Stage 1 (host): set up mounts and hand off to the container.
# ---------------------------------------------------------------------------
if ! in_container; then
    repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

    engine="${KEYBALL39_ENGINE:-}"
    if [ -z "$engine" ]; then
        if command -v podman >/dev/null 2>&1; then engine=podman
        elif command -v docker >/dev/null 2>&1; then engine=docker
        else echo "error: neither podman nor docker found on PATH" >&2; exit 1
        fi
    fi

    if [ ! -d "$repo/zmk" ] || [ ! -d "$repo/zephyr" ]; then
        echo "error: west workspace not initialised (no zmk/ or zephyr/)." >&2
        echo "       run: tools/west-init.sh" >&2
        exit 1
    fi

    mkdir -p "$repo/.ccache" "$repo/.build" "$repo/firmware"

    # ccache reads $CCACHE_DIR/ccache.conf. Zephyr invokes the compiler with
    # `-specs=picolibc.specs` (a relative path resolved via the compiler's own
    # search dirs); ccache tries to lstat it from the build CWD, fails, and
    # marks every translation unit `bad_compiler_arguments` -- so the cache
    # stays permanently empty. Ignoring those options when hashing is safe
    # because the spec files are frozen inside the image.
    if [ ! -f "$repo/.ccache/ccache.conf" ]; then
        cat >"$repo/.ccache/ccache.conf" <<'EOF'
ignore_options = -specs=*
sloppiness = include_file_mtime,include_file_ctime,time_macros
max_size = 5G
EOF
    fi

    tty_args=()
    [ -t 1 ] && tty_args=(-t)

    exec "$engine" run --rm "${tty_args[@]}" \
        -v "$repo:$WS" \
        -v "$repo/.ccache:/ccache" \
        -w "$WS" \
        -e CCACHE_DIR=/ccache \
        -e CCACHE_BASEDIR="$WS" \
        "$IMAGE" \
        "$WS/tools/build.sh" "$@"
fi

# ---------------------------------------------------------------------------
# Stage 2 (container): parse build.yaml and build.
# ---------------------------------------------------------------------------
pristine=0
filters=()
west_extra=()

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--pristine) pristine=1 ;;
        --) shift; west_extra=("$@"); break ;;
        -h|--help) sed -n '2,25p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) filters+=("$1") ;;
    esac
    shift
done

[ ${#filters[@]} -eq 0 ] && filters=(".")

# Emit one record per target: board, shield, snippet, artifact, cmake-args.
#
# Fields are separated by ASCII US (0x1f), NOT tab. Bash's `read` treats runs
# of IFS *whitespace* (space/tab/newline) as a single delimiter, so a target
# with no snippet would collapse two tabs into one and silently shift every
# later field left -- which is how the artifact name ended up being passed to
# `-S`. A non-whitespace separator preserves empty fields.
targets=$(python3 - "$WS/build.yaml" <<'PY'
import sys, yaml
doc = yaml.safe_load(open(sys.argv[1])) or {}
rows = []
for e in (doc.get("include") or []):
    board   = e.get("board", "")
    shield  = e.get("shield", "")
    snippet = e.get("snippet", "")
    cmake   = e.get("cmake-args", "")
    art     = e.get("artifact-name") or "-".join(
        p for p in (shield.replace(" ", "+"), board.replace("/", "_")) if p)
    rows.append("\x1f".join((board, shield, snippet, art, cmake)))
print("\n".join(rows))
PY
)

[ -z "$targets" ] && { echo "error: no targets in build.yaml" >&2; exit 1; }

# Keep only targets matching any filter (substring match over the whole row).
selected=$(printf '%s\n' "$targets" | grep -E "$(IFS='|'; echo "${filters[*]}")" || true)
if [ -z "$selected" ]; then
    echo "error: no targets matched: ${filters[*]}" >&2
    echo "available:" >&2
    printf '%s\n' "$targets" | cut -d$'\x1f' -f4 | sed 's/^/  /' >&2
    exit 1
fi

west zephyr-export >/dev/null 2>&1 || true

built=()
while IFS=$'\x1f' read -r board shield snippet artifact cmake_args; do
    [ -z "$board" ] && continue
    build_dir="$WS/.build/$artifact"

    [ "$pristine" -eq 1 ] && rm -rf "$build_dir"

    echo
    echo "=== $artifact ==============================================="

    if [ -f "$build_dir/build.ninja" ]; then
        # Incremental: re-passing -b/-S/-D forces a full CMake reconfigure even
        # when nothing changed, which is by far the slowest phase. Pointing at
        # an existing build dir lets ninja do just the work that is needed.
        echo "--- incremental (use -p to force a clean configure)"
        west build -d "$build_dir" "${west_extra[@]}"
    else
        echo "--- configuring from scratch"
        # shellcheck disable=SC2086
        west build -s zmk/app -d "$build_dir" -b "$board" \
            ${snippet:+-S "$snippet"} "${west_extra[@]}" -- \
            -DZMK_CONFIG="$WS/config" ${shield:+-DSHIELD="$shield"} $cmake_args
    fi

    if [ -f "$build_dir/zephyr/zmk.uf2" ]; then
        cp "$build_dir/zephyr/zmk.uf2" "$WS/firmware/$artifact.uf2"
        built+=("firmware/$artifact.uf2")
    else
        cp "$build_dir/zephyr/zmk.bin" "$WS/firmware/$artifact.bin"
        built+=("firmware/$artifact.bin")
    fi
done <<<"$selected"

echo
echo "=== artifacts ==============================================="
for f in "${built[@]}"; do
    printf '  %-46s %s\n' "$f" "$(du -h "$WS/$f" | cut -f1)"
done
echo
ccache -s 2>/dev/null | grep -Ei 'cache hit|cache miss|hits' | sed 's/^/  ccache: /' || true
