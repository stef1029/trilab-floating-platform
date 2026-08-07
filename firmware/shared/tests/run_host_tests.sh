#!/usr/bin/env sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=${TMPDIR:-/tmp}/fairy_protocol_tests
mkdir -p "$build_dir"

c++ -std=c++17 -Wall -Wextra -Werror -pedantic \
  -I"$root_dir/shared/include" \
  "$root_dir/shared/src/transport.cpp" \
  "$root_dir/shared/src/fairy_protocol.cpp" \
  "$root_dir/shared/src/adelie_protocol.cpp" \
  "$root_dir/shared/src/magellan_protocol.cpp" \
  "$root_dir/shared/tests/protocol_tests.cpp" \
  -o "$build_dir/protocol_tests"

"$build_dir/protocol_tests"
