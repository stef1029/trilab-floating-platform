#!/usr/bin/env sh
# Build the Nordic (Zephyr) firmware without the toolchain on PATH.
#
#   ./build_nordic.sh korora
#   ./build_nordic.sh galapagos
#   ./build_nordic.sh korora -- -t menuconfig     # extra west args after --
#
# Korora must target the u-blox BMD-340 eval board: the plain nrf52840dk
# overlay does not define rs485-dir-gpios, which src/rs485_bus.cpp requires.
set -eu

NCS_VERSION=${NCS_VERSION:-v3.4.1}
NRFUTIL=${NRFUTIL:-/c/ncs/tools/nrfutil.exe}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

app=${1:-}
case "$app" in
  korora)    board=ubx_bmd340eval/nrf52840 ;;
  galapagos) board=nrf54l15dk/nrf54l15/cpuapp ;;
  *) echo "usage: $0 {korora|galapagos} [-- <extra west args>]" >&2; exit 2 ;;
esac
shift
[ "${1:-}" = "--" ] && shift || true

ZEPHYR_BASE="C:/ncs/${NCS_VERSION}/zephyr"
export ZEPHYR_BASE

cd "$here"
echo "Building $app for $board  (NCS $NCS_VERSION)"
exec "$NRFUTIL" sdk-manager toolchain launch --ncs-version "$NCS_VERSION" -- \
  west build -p always -b "$board" "$app" --build-dir "$app/build" "$@"
