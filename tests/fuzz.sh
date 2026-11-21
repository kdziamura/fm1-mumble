#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Build the MUMBLE firmware's fuzzer (tests/fuzz.c) with clang, ASan and UBSan,
# then fuzz into app/build/fuzz/ (corpus, crashes) from the seeds. Arguments go
# to libFuzzer: e.g. -max_total_time=600 -jobs=8, or a crash file to replay it.
#   tests/fuzz.sh --replay [FILES]   every seed and corpus input (or FILES), each
#                                    in its own run of the build whose globals
#                                    ASan guards (FUZZ_ASAN_GLOBALS)
set -e
cd "$(dirname "$0")/.."
CC="${CC:-clang}"
out=app/build/fuzz
mkdir -p "$out/corpus" "$out/crashes"
build() {
    $CC -std=c11 -O1 -g -fwrapv -ffp-contract=off -Wall -Wextra -Werror \
        -Wno-unused-function -Wno-unused-parameter \
        -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
        -Iapp -Ibase/include "$@" tests/fuzz.c tests/fuzz_engine.c -lm
}
if [ "${1:-}" = --replay ]; then
    shift
    build -DFUZZ_ASAN_GLOBALS -o "$out/mumble-fuzz-globals"
    [ $# -gt 0 ] || set -- tests/fuzz-seeds/* "$out"/corpus/*
    n=0
    for f in "$@"; do
        [ -f "$f" ] || continue
        "$out/mumble-fuzz-globals" "$f" >"$out/replay.log" 2>&1 || { cat "$out/replay.log"; echo "replay: $f FAILED"; exit 1; }
        n=$((n + 1))
    done
    echo "replay: $n inputs, globals guarded: ok"
    exit 0
fi
build -o "$out/mumble-fuzz"
[ "${FUZZ_BUILD_ONLY:-}" ] && exit 0
cp -n tests/fuzz-seeds/* "$out/corpus/" 2>/dev/null || true
exec "$out/mumble-fuzz" "$out/corpus" -max_len=1024 -timeout=60 -rss_limit_mb=4096 \
    -artifact_prefix="$out/crashes/" "$@"
