#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Build and run the engine's host test (tests/engine_test.c) against app/mumble.c
# on the host platform; with a directory, it also writes each character's WAV there.
#   tests/engine.sh [WAV_DIR]
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
mkdir -p app/build
$CC -std=c11 -O2 -Wall -Wextra -Werror -fwrapv -ffp-contract=off -DFM1_HOST_TEST -DPLAT_HOST=1 \
    -Iapp -Ibase/include app/engine.c tests/engine_test.c -lm -o app/build/engine-test
[ $# -gt 0 ] && mkdir -p "$1"
app/build/engine-test "$@"
