#!/usr/bin/env bash
# Build (optionally) and publish a firmware release to the OTA server.
#
#   ./scripts/deploy-firmware.sh              # build, then publish
#   ./scripts/deploy-firmware.sh --no-build   # publish what is already built
#   ./scripts/deploy-firmware.sh --dry-run    # show what would happen
#
# Publishes to <host>:<root>/<model>/ as:
#     <model>-<version>.bin     the image
#     latest.json               {"version", "file", "sha256", "size"}
#
# The image is uploaded first and the manifest second, so a device fetching
# mid-deploy never sees a manifest pointing at a file that is not there yet.
set -euo pipefail

OTA_HOST="${OTA_HOST:-ota}"
OTA_ROOT="${OTA_ROOT:-/srv/ota}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDKCONFIG="$PROJECT_DIR/sdkconfig"

build=1
dry_run=0
for arg in "$@"; do
    case "$arg" in
        --no-build) build=0 ;;
        --dry-run)  dry_run=1 ;;
        -h|--help)  sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

cfg() {   # cfg CONFIG_NAME -> value with quotes stripped
    local v
    v="$(grep -E "^$1=" "$SDKCONFIG" | head -1 | cut -d= -f2-)" || true
    printf '%s' "${v%\"}" | sed 's/^"//'
}

[ -f "$SDKCONFIG" ] || { echo "no sdkconfig; run a build first" >&2; exit 1; }

VERSION="$(cfg CONFIG_APP_PROJECT_VER)"
MODEL="$(cfg CONFIG_OTA_MODEL)"
SIGNED="$(cfg CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES)"

[ -n "$VERSION" ] || { echo "CONFIG_APP_PROJECT_VER is empty" >&2; exit 1; }
[ -n "$MODEL" ]   || { echo "CONFIG_OTA_MODEL is empty" >&2; exit 1; }
[ "$SIGNED" = "y" ] || echo "warning: images are not signed (CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES)" >&2

cd "$PROJECT_DIR"

# One trap for everything: a second `trap ... EXIT` would silently replace the
# first, and the signing key must be shredded even if the build fails.
STAGE=""
fetched_key=0
cleanup() {
    [ "$fetched_key" = 1 ] && "$PROJECT_DIR/scripts/ota-key.sh" shred || true
    [ -n "$STAGE" ] && rm -rf "$STAGE" || true
}
trap cleanup EXIT

if [ "$build" = 1 ] && [ "$dry_run" = 0 ]; then
    "$PROJECT_DIR/scripts/ota-key.sh" fetch
    fetched_key=1
    idf.py build
fi

# The app image is named in the build metadata; globbing build/*.bin would
# also match ota_data_initial.bin and friends.
APP_BIN="$(python3 -c "import json;print(json.load(open('build/project_description.json'))['app_bin'])" 2>/dev/null || true)"
BIN="$PROJECT_DIR/build/$APP_BIN"
[ -n "$APP_BIN" ] && [ -f "$BIN" ] || { echo "no firmware binary in build/ (run a build first)" >&2; exit 1; }

STAGE="$(mktemp -d)"
RELEASE="$MODEL-$VERSION.bin"
cp "$BIN" "$STAGE/$RELEASE"

SHA="$(sha256sum "$STAGE/$RELEASE" | cut -d' ' -f1)"
SIZE="$(stat -c %s "$STAGE/$RELEASE")"
cat > "$STAGE/latest.json" <<JSON
{
  "version": "$VERSION",
  "file": "$RELEASE",
  "sha256": "$SHA",
  "size": $SIZE
}
JSON

echo "model:   $MODEL"
echo "version: $VERSION"
echo "file:    $RELEASE ($SIZE bytes)"
echo "sha256:  $SHA"
echo "target:  $OTA_HOST:$OTA_ROOT/$MODEL/"

if [ "$dry_run" = 1 ]; then
    echo "--- latest.json ---"
    cat "$STAGE/latest.json"
    echo "(dry run, nothing uploaded)"
    exit 0
fi

# Image first, manifest second.
rsync -av "$STAGE/$RELEASE" "$OTA_HOST:$OTA_ROOT/$MODEL/"
rsync -av "$STAGE/latest.json" "$OTA_HOST:$OTA_ROOT/$MODEL/"

echo "published $MODEL $VERSION"
