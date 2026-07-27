#!/usr/bin/env bash
#
# Fetch the pluginval build CI uses into tools/bin/ (gitignored).
#
# Pinned to whatever Tracktion currently ships as "latest", which is what the workflow also
# resolves, so a local pass means the same binary agreed.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${REPO_ROOT}/tools/bin"

command -v gh >/dev/null || { echo "gh CLI is required to resolve the download URL" >&2; exit 1; }

URL="$(gh api repos/Tracktion/pluginval/releases/latest \
        --jq '.assets[] | select(.name == "pluginval_macOS.zip") | .browser_download_url')"

[[ -n "$URL" ]] || { echo "could not resolve pluginval_macOS.zip URL" >&2; exit 1; }

echo "Downloading ${URL}"
mkdir -p "$DEST"
cd "$DEST"
curl -sSfL -o pluginval.zip "$URL"
rm -rf pluginval.app
ditto -x -k pluginval.zip .
rm pluginval.zip
chmod +x pluginval.app/Contents/MacOS/pluginval

# Downloaded zips carry the quarantine flag, which would make the first run prompt.
xattr -dr com.apple.quarantine pluginval.app 2>/dev/null || true

echo "Installed: $("$DEST/pluginval.app/Contents/MacOS/pluginval" --version)"
