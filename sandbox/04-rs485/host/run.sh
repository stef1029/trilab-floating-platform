#!/usr/bin/env sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PYTHONPATH="$here/../../../software/src"   exec "$here/../../../software/.venv/Scripts/python.exe" "$here/fairy_led.py" "$@"
