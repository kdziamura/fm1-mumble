#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Build MUMBLE (fm1-base's tools/build.py on app/): the arguments go to it as they are.
#   ./build.sh                      app/build/mumble.fwsc, a dev build (JieLi toolchain, AC79 SDK)
#   ./build.sh --release 0.1.0      app/build/mumble-0.1.0.fwsc, identity FM-1_68000100, version 0.1.0
#   ./build.sh --host               the host simulator: app/build/host/mumble_host
#   ./build.sh --web                the browser emulator: app/build/emu/ (Emscripten)
#   ./build.sh --rfm1 --release 0.1.0  app/build/mumble-0.1.0.rfm1, for host apps (RemoteFM1)
# PYTHON: the Python to run it with (default: fm1-base's .venv if there is one, else python3).
set -e
cd "$(dirname "$0")"
if [ -z "${PYTHON:-}" ]; then
    if [ -x base/.venv/bin/python ]; then PYTHON=base/.venv/bin/python; else PYTHON=python3; fi
fi
exec "$PYTHON" base/tools/build.py app "$@"
