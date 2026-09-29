#!/usr/bin/env sh
# Build the Nordic (Zephyr) firmware without the toolchain on PATH.
#
#   ./build_nordic.sh korora              # BMD-340 module / hub PCB
#   ./build_nordic.sh korora dk           # nRF52840 DK breadboard rig
#   ./build_nordic.sh galapagos           # nRF54L15 DK
#   ./build_nordic.sh korora dk -- -t menuconfig    # extra west args after --
#
# Each target gets its own build directory named after the board, so the
# variants coexist and incremental rebuilds stay fast:
#
#   korora/build-bmd340eval     korora/build-dk     galapagos/build-dk
set -eu

NCS_VERSION=${NCS_VERSION:-v3.4.1}
NRFUTIL=${NRFUTIL:-/c/ncs/tools/nrfutil.exe}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

usage() {
  echo "usage: $0 {korora [dk] | galapagos} [-- <extra west args>]" >&2
  exit 2
}

app=${1:-}
[ -n "$app" ] || usage
shift

variant=""
if [ "${1:-}" != "--" ] && [ -n "${1:-}" ]; then
  variant=$1
  shift
fi
[ "${1:-}" = "--" ] && shift || true

case "$app:$variant" in
  korora:)      board=ubx_bmd340eval/nrf52840;    build=build-bmd340eval ;;
  korora:pcb)   board=ubx_bmd340eval/nrf52840;    build=build-bmd340eval ;;
  korora:dk)    board=nrf52840dk/nrf52840;        build=build-dk ;;
  galapagos:)   board=nrf54l15dk/nrf54l15/cpuapp; build=build-dk ;;
  galapagos:dk) board=nrf54l15dk/nrf54l15/cpuapp; build=build-dk ;;
  *) usage ;;
esac

ZEPHYR_BASE="C:/ncs/${NCS_VERSION}/zephyr"
export ZEPHYR_BASE

cd "$here"
echo "Building $app for $board  ->  $app/$build   (NCS $NCS_VERSION)"
exec "$NRFUTIL" sdk-manager toolchain launch --ncs-version "$NCS_VERSION" -- \
  west build -p always -b "$board" "$app" --build-dir "$app/$build" "$@"
