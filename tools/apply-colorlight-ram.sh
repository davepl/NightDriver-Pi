#!/bin/sh
# Restore the eight-panel FM6124 receiver's volatile settings before playback.
set -eu

ndpi_bin_dir=${NDPI_BIN_DIR:-/opt/ndpi/bin}
ndpi_bundle=${NDPI_RAM_BUNDLE:-/etc/ndpi/fm6124-512x32-runtime-complete.ndram}
ndpi_config=${NDPI_CONFIG:-/etc/ndpi/colorlight.json}

if [ ! -r "$ndpi_bundle" ]; then
    printf '%s\n' "Missing this receiver's RAM bundle: $ndpi_bundle" >&2
    exit 1
fi

for ndpi_pass in 1 2 3; do
    "$ndpi_bin_dir/colorlight-ram-config" --packets "$ndpi_bundle" \
        --config "$ndpi_config" --apply-ram --packet-gap-ms=100
    sleep 1
done
