#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Build the MUMBLE firmware's fuzzer (tests/fuzz.c) with clang, ASan and UBSan,
# then fuzz into app/build/fuzz/ (corpus, crashes) from the seeds. Arguments go
# to libFuzzer: e.g. -max_total_time=600 -jobs=8, or a crash file to replay it.
#   tests/fuzz.sh --replay [FILES]   every seed and corpus input (or FILES), each
#                                    in its own run of the build whose globals
#                                    ASan guards (FUZZ_ASAN_GLOBALS)
#   tests/fuzz.sh --explain FILE     FILE as the panel actions it plays (and how it fails)
#   tests/fuzz.sh --minimize FILE    a failing input shrunk (libFuzzer's crash minimizer, at most
#                                    two minutes) into FILE.min, then explained
#   tests/fuzz.sh --merge            the corpus shrunk to the smallest set of inputs with the
#                                    same coverage (app/build/fuzz/corpus, the old one kept as corpus.old)
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
case "${1:-}" in
--explain)
    MUMBLE_FUZZ_EXPLAIN=1 "$out/mumble-fuzz" "$2" 2>&1 | grep -v '^INFO\|^Running\|^Executed\|^ *#[0-9]\|^==\|^NOTE\|^SUMMARY\|^MS:\|^base unit\|^0x\|^artifact_prefix\|^Test unit\|^\*\*\*\|: Running \|Combine libFuzzer\|^$'
    exit 0 ;;
--minimize)
    "$out/mumble-fuzz" -minimize_crash=1 -max_total_time=120 -exact_artifact_path="$2.min" "$2" >"$out/minimize.log" 2>&1 || true
    [ -f "$2.min" ] || { tail -n 20 "$out/minimize.log"; exit 1; }
    echo "$2.min: $(wc -c <"$2.min") bytes (from $(wc -c <"$2"))"
    exec "$0" --explain "$2.min" ;;
--merge)
    rm -rf "$out/corpus.old" "$out/corpus.new"
    mkdir -p "$out/corpus.new"
    "$out/mumble-fuzz" -merge=1 -timeout=60 "$out/corpus.new" "$out/corpus" tests/fuzz-seeds >"$out/merge.log" 2>&1
    mv "$out/corpus" "$out/corpus.old" && mv "$out/corpus.new" "$out/corpus"
    echo "corpus: $(ls "$out/corpus.old" | wc -l) -> $(ls "$out/corpus" | wc -l) inputs"
    exit 0 ;;
esac
cp -n tests/fuzz-seeds/* "$out/corpus/" 2>/dev/null || true
exec "$out/mumble-fuzz" "$out/corpus" -max_len=1024 -timeout=60 -rss_limit_mb=4096 \
    -artifact_prefix="$out/crashes/" "$@"
