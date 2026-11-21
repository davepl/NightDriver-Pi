#!/bin/sh
# Apply this host's prepared, temporary FM6124 settings, then run playback.
# The receiver loses these RAM settings on a power cycle. No flash is saved.
set -eu

ndpi_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ndpi_repo"

ndpi_config=examples/colorlight-mac-512x32.json
if [ "${1:-}" = --lan ]; then
    shift
    ndpi_config=examples/colorlight-mac-lan-512x32.json
fi

ndpi_bundle=${NDPI_RAM_BUNDLE:-receiver-backups/fm6124-512x32-runtime-complete.ndram}
if [ ! -r "$ndpi_bundle" ]; then
    printf '%s\n' "Missing this receiver's locally generated RAM bundle: $ndpi_bundle" >&2
    printf '%s\n' "See docs/colorlight.md; the vendor-generated panel data is not distributed with the source." >&2
    exit 1
fi
if lsof -t -nP -iTCP:49153 -sTCP:LISTEN >/dev/null 2>&1; then
    printf '%s\n' 'TCP port 49153 is already in use. Stop the running display process first.' >&2
    exit 1
fi

make -s all ram-config
printf '%s\n' "Applying temporary FM6124 settings for eight 64x32 panels using $ndpi_config."
for ndpi_pass in 1 2 3; do
    build/tools/colorlight-ram-config --packets "$ndpi_bundle" --config "$ndpi_config" --apply-ram --packet-gap-ms=100
    sleep 1
done
exec ./ndpi --output=colorlight --config="$ndpi_config" --port=49153 "$@"
