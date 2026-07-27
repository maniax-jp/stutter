#!/usr/bin/env bash
#
# Local plugin-format validation: the gates CI runs, runnable before pushing a tag.
#
# Relying on CI alone cost a full release cycle -- v2.0.0's first build failed at pluginval
# after every local test had passed. This closes that gap for both formats.
#
# auval needs the AU to be registered with Core Audio, which can silently stop working until
# the machine is restarted: the AU registry once reported only Apple's own components here
# while dozens of installed commercial plugins were equally invisible. The script tells that
# state apart from a real rejection rather than blaming the plugin for it.
#
# Usage: tools/validate.sh [build-dir]

set -euo pipefail

BUILD_DIR="${1:-build}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PLUGINVAL="tools/bin/pluginval.app/Contents/MacOS/pluginval"
VST3="${BUILD_DIR}/Stutter_artefacts/Release/VST3/Stutter.vst3"
AU="${BUILD_DIR}/Stutter_artefacts/Release/AU/Stutter.component"

# pluginval's own timeout for any single test. CI's runner is slower than a dev machine, so a
# test that merely gets slow here can time out there -- see the elapsed-time report below.
STRICTNESS="${STRICTNESS:-8}"

fail() { echo "FAIL: $*" >&2; exit 1; }

# ---- pluginval -------------------------------------------------------------------------
if [[ ! -x "$PLUGINVAL" ]]; then
    cat >&2 <<'MSG'
pluginval is not installed. Fetch it with:

  tools/fetch-pluginval.sh

MSG
    exit 1
fi

[[ -d "$VST3" ]] || fail "VST3 not found at ${VST3} -- build first"

echo "==> pluginval --strictness-level ${STRICTNESS}"
started=$(date +%s)
if ! "$PLUGINVAL" --strictness-level "$STRICTNESS" --validate "$VST3"; then
    fail "pluginval rejected the VST3"
fi
elapsed=$(( $(date +%s) - started ))

echo
echo "pluginval passed in ${elapsed}s."

# A pass is not the whole signal. The v2.0.0 failure was a 30s-per-test timeout that this
# machine was fast enough to survive: the same build took 45s here and still passed, versus
# 16s once fixed. A large number here means CI may still fail even though this did not.
if (( elapsed > 30 )); then
    echo
    echo "WARNING: that is slow. A healthy run is well under 30s; the v2.0.0 regression"
    echo "         measured 45s here and still timed out on CI. Investigate before tagging."
fi

# ---- auval -----------------------------------------------------------------------------
echo
echo "==> auval -v aufx Stt1 Manx"

if [[ ! -d "$AU" ]]; then
    echo "SKIP: AU not built at ${AU}"
    exit 0
fi

# auval only sees components under ~/Library/Audio/Plug-Ins/Components.
AU_DEST="$HOME/Library/Audio/Plug-Ins/Components/Stutter.component"
ditto "$AU" "$AU_DEST"
killall -9 AudioComponentRegistrar 2>/dev/null || true
sleep 3

if auval -v aufx Stt1 Manx >/dev/null 2>&1; then
    echo "auval passed."
else
    # Distinguish "our plugin is broken" from "no AU registers on this machine" by checking
    # whether ANY third-party AU is visible. Without this the message would blame the plugin.
    third_party=$(auval -a 2>/dev/null | grep -E "^au" | awk '{print $3}' | grep -cv appl || true)
    if [[ "$third_party" -eq 0 ]]; then
        echo "SKIP: Core Audio has no third-party AU registered at all (only Apple's), so"
        echo "      every installed plugin is equally invisible and this says nothing about"
        echo "      Stutter. Restarting the machine has fixed this before; killing"
        echo "      AudioComponentRegistrar and clearing the AU cache did not."
    else
        fail "auval rejected the AU (${third_party} third-party AUs do register here, so this is real)"
    fi
fi
