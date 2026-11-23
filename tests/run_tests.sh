#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# MUMBLE's tests, all but the fuzzer (.github/workflows/mumble-fm1.yml runs that): the engine, the faces (C
# against Python), and the host simulator's scripts. The release builder runs this; a failure stops the release.
#   tests/run_tests.sh              (PYTHON: as ./build.sh; CC: the host's C compiler, default cc)
set -e
cd "$(dirname "$0")/.."
if [ -z "${PYTHON:-}" ]; then
    if [ -x base/.venv/bin/python ]; then PYTHON=base/.venv/bin/python; else PYTHON=python3; fi
fi
export PYTHON
tests/engine.sh | tail -n 1
tests/faces.sh "$PYTHON"
./build.sh --host
for t in basic record line controls save clock; do
    app/build/host/mumble_host "app/tests/$t.txt" app/build/host > "app/build/host/$t.log" 2>&1 \
        || { grep -v '^  ok' "app/build/host/$t.log"; echo "host: app/tests/$t.txt failed"; exit 1; }
    echo "host: app/tests/$t.txt ok"
done
