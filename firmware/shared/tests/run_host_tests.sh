#!/usr/bin/env sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=${TMPDIR:-/tmp}/record_protocol_tests
mkdir -p "$build_dir"

c++ -std=c++17 -Wall -Wextra -Werror -pedantic \
  -I"$root_dir/shared/include" \
  "$root_dir/shared/src/transport.cpp" \
  "$root_dir/shared/src/record_protocol.cpp" \
  "$root_dir/shared/src/command_protocol.cpp" \
  "$root_dir/shared/src/discovery_protocol.cpp" \
  "$root_dir/shared/tests/protocol_tests.cpp" \
  -o "$build_dir/protocol_tests"

"$build_dir/protocol_tests"
