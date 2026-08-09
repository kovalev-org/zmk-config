#!/usr/bin/env bash
#
# Restore executable bits across the west workspace.
#
# Windows filesystems don't carry the POSIX executable bit, so a workspace
# copied from a Windows host arrives with every script mode 0644. The build
# then dies at the first generator it tries to exec, e.g.:
#
#   /bin/sh: 1: .../modules/lib/nanopb/generator/protoc: Permission denied
#
# For every git repo in the workspace this re-applies +x to exactly the files
# git already records as mode 100755. It only adds the bit -- file contents and
# any local edits are untouched -- so it is safe to re-run.
#
# Runs on the host; needs nothing but git.

set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo"

total=0
for gitdir in $(find . zmk zephyr modules -maxdepth 4 -name .git -printf '%h\n' 2>/dev/null | sort -u); do
    [ -d "$gitdir" ] || continue
    fixed=0
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        path="$gitdir/$f"
        if [ -f "$path" ] && [ ! -x "$path" ]; then
            chmod +x "$path"
            fixed=$((fixed + 1))
        fi
    done < <(git -C "$gitdir" ls-files --stage 2>/dev/null | awk '$1=="100755"{ $1=""; $2=""; $3=""; sub(/^[ \t]+/, ""); print }')

    if [ "$fixed" -gt 0 ]; then
        printf '  %-40s %4d restored\n' "${gitdir#./}" "$fixed"
        total=$((total + fixed))
    fi
done

echo
if [ "$total" -eq 0 ]; then
    echo "nothing to do -- all executable bits already correct."
else
    echo "restored the executable bit on $total file(s)."
fi
