#!/bin/sh
# Launch our eight-panel 512x32 Colorlight matrix over the switched LAN (en35).
set -eu

ndpi_repo=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$ndpi_repo/tools/run-colorlight-mac.sh" --lan "$@"
