#!/usr/bin/env sh
# Apply this project's local patches to the psxrecomp submodule.
#
# The submodule is upstream code we don't control, so changes live here as
# patches rather than as a fork. Re-run after `git submodule update`, which
# discards them.
#
#   scripts/apply-patches.sh          apply (idempotent; skips already-applied)
#   scripts/apply-patches.sh --revert restore the submodule to pristine

set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SUB="$ROOT/psxrecomp"
git -C "$SUB" rev-parse --git-dir >/dev/null 2>&1 || \
    { echo "error: $SUB is not a git checkout" >&2; exit 1; }

if [ "${1:-}" = "--revert" ]; then
    git -C "$SUB" checkout -- .
    echo "psxrecomp restored to pristine."
    exit 0
fi

for p in "$ROOT"/patches/*.patch; do
    [ -e "$p" ] || continue
    name=$(basename "$p")
    if git -C "$SUB" apply --reverse --check "$p" 2>/dev/null; then
        echo "  already applied: $name"
    elif git -C "$SUB" apply "$p" 2>/dev/null; then
        echo "  applied:         $name"
    else
        echo "  FAILED:          $name" >&2
        echo "    Upstream has probably moved. Rebase the patch, or drop it if" >&2
        echo "    the change has landed upstream." >&2
        exit 1
    fi
done
