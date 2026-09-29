#!/usr/bin/env bash
# Fetch the OTA image-signing key from 1Password into tmpfs for a build, and
# shred it afterwards, so it is never written to disk.
#
#   ota-key.sh fetch    # before building
#   ota-key.sh shred    # after building/flashing
#
# The path must match CONFIG_SECURE_BOOT_SIGNING_KEY in sdkconfig.
set -euo pipefail

KEY_PATH="${OTA_SIGNING_KEY:-/run/user/$(id -u)/ota_key.pem}"
OP_ITEM="${OTA_SIGNING_KEY_ITEM:-WS2812BClock OTA signing key}"

case "${1:-}" in
    fetch)
        if [ -f "$KEY_PATH" ]; then
            echo "signing key already present at $KEY_PATH"
            exit 0
        fi
        # Fail fast rather than hang: in a VS Code task there is nowhere to
        # show an interactive prompt, so an unauthenticated op would block
        # the build forever.
        if ! timeout 10 op whoami >/dev/null 2>&1; then
            echo "1Password is not signed in, and this task cannot prompt." >&2
            echo "Run this in a terminal first:  ./scripts/ota-key.sh fetch" >&2
            echo "(or fix the desktop integration: restart the 1Password app," >&2
            echo " enable Settings > Developer > Integrate with 1Password CLI)" >&2
            exit 1
        fi
        timeout 60 op document get "$OP_ITEM" --out-file "$KEY_PATH"
        chmod 600 "$KEY_PATH"
        echo "signing key fetched to $KEY_PATH"
        ;;
    shred)
        if [ -f "$KEY_PATH" ]; then
            shred -u "$KEY_PATH"
            echo "signing key shredded"
        fi
        ;;
    *)
        echo "usage: $0 {fetch|shred}" >&2
        exit 2
        ;;
esac
