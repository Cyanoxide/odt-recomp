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

# Always start from pristine and replay the whole series. Patches can stack on
# one file, and "is this one already applied?" is unanswerable once a later
# patch sits on an earlier one's context. Replaying is deterministic instead.
# NOTE: this discards uncommitted edits inside psxrecomp - make them patches.
git -C "$SUB" checkout -- .

for p in "$ROOT"/patches/*.patch; do
    [ -e "$p" ] || continue
    name=$(basename "$p")
    if git -C "$SUB" apply "$p" 2>/dev/null; then
        echo "  applied:  $name"
    else
        echo "  FAILED:   $name" >&2
        echo "    Upstream has probably moved, or an earlier patch in the" >&2
        echo "    series changed its context. Rebase it (git apply --3way)," >&2
        echo "    or drop it if the change has landed upstream." >&2
        exit 1
    fi
done
