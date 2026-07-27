#!/usr/bin/env bash
# Verify the current build's per-lane renders against the v1.1.2 golden baseline.
#
# This is the regression gate for WP1 (effect contract migration) and WP3 (BlockSequencer).
# Both are expected to be bit-identical to v1: WP1 only changes where parameters come from,
# and WP3 with beats=4/divisions=4/swing=0 and one block per step must reproduce v1's
# fixed 16-step grid exactly. A mismatch means the DSP changed, which for those two work
# packages is a bug, not progress.
#
# Usage:  tests/fixtures/golden/verify-against-v1.sh [path-to-render_test]
# Default binary: build/tools/render_test/render_test
#
# Exit 0 = all lanes bit-identical to v1.1.2. Exit 1 = at least one lane differs.

set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../.." && pwd)"
render_test="${1:-$repo_root/build/tools/render_test/render_test}"
checksums="$script_dir/v1-lane-renders.sha256"

if [[ ! -x "$render_test" ]]; then
    echo "error: render_test not found or not executable: $render_test" >&2
    echo "hint: cmake -B build -DCMAKE_BUILD_TYPE=Release -DSTUTTER_BUILD_TESTS=ON" >&2
    echo "      cmake --build build --target render_test -j8" >&2
    exit 1
fi

if [[ ! -f "$checksums" ]]; then
    echo "error: golden checksums missing: $checksums" >&2
    exit 1
fi

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

echo "render_test: $render_test"
echo "baseline:    $checksums"
echo

# render_test resolves its output dir relative to the current working directory.
if ! (cd "$workdir" && "$render_test" out > "$workdir/render.log" 2>&1); then
    echo "note: render_test reported failures of its own (see log below)."
    echo "      The golden comparison still runs -- a v1 mismatch and a failing"
    echo "      self-check are different signals and you want to see both."
    echo
    tail -30 "$workdir/render.log"
    echo
fi

if ! (cd "$workdir/out" && shasum -a 256 -c "$checksums" --status 2>/dev/null); then
    echo "MISMATCH vs v1.1.2 golden baseline:"
    echo
    (cd "$workdir/out" && shasum -a 256 -c "$checksums" 2>&1) | sed 's/^/  /'
    echo
    echo "Current metrics (compare against v1-lane-metrics.txt):"
    grep -E "^(Stutter|TapeStop|TapeStart|Reverse|Repitch|Gate|Filter|Crush) " \
        "$workdir/render.log" 2>/dev/null | sed 's/^/  /' || echo "  (metrics table not found in log)"
    exit 1
fi

echo "OK: all 8 lanes bit-identical to the v1.1.2 baseline."
