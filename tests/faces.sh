#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# The firmware's face generator (app/mumble_faces.inc) against tools/faces.py:
# both draw the same faces, icons and rigs for the cast, 400 rolled voices and
# every setting at both ends, or the difference is printed.
#   tests/faces.sh [PYTHON]      (default: base/.venv/bin/python)
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
PY="${1:-base/.venv/bin/python}"
mkdir -p app/build
$CC -std=c11 -O2 -Wall -Wextra -Werror -fwrapv -DFM1_HOST_TEST -DPLAT_HOST=1 \
    -Iapp -Ibase/include app/engine.c tests/faces_test.c -lm -o app/build/faces-test
app/build/faces-test >app/build/faces-c.txt
"$PY" tools/faces.py --dump >app/build/faces-py.txt
if cmp -s app/build/faces-c.txt app/build/faces-py.txt; then
    echo "faces: C and Python agree on $(grep -c '^mouth' app/build/faces-c.txt) faces"
else
    diff app/build/faces-py.txt app/build/faces-c.txt | head -40
    echo "faces: C and Python differ"
    exit 1
fi
