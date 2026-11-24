#!/usr/bin/env sh
# Run the sandbox BLE host using the Adelie virtual environment.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$here/../../../software/.venv/Scripts/python.exe" "$here/led_button.py" "$@"
