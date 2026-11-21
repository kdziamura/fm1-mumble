#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Build the MUMBLE firmware's fuzzer (tests/fuzz.c) with clang, ASan and UBSan,
# then fuzz into build/fuzz/ (corpus, crashes) from the seeds. Arguments go to
# libFuzzer: e.g. -max_total_time=600 -jobs=8, or a crash file to replay it.
set -e
cd "$(dirname "$0")/.."
CC="${CC:-clang}"
out=app/build/fuzz
mkdir -p "$out/corpus" "$out/crashes"
$CC -std=c11 -O1 -g -fwrapv -ffp-contract=off -Wall -Wextra -Werror \
    -Wno-unused-function -Wno-unused-parameter \
    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
    -Iapp -Ibase/include tests/fuzz.c tests/fuzz_engine.c -lm -o "$out/mumble-fuzz"
[ "${FUZZ_BUILD_ONLY:-}" ] && exit 0
cp -n tests/fuzz-seeds/* "$out/corpus/" 2>/dev/null || true
exec "$out/mumble-fuzz" "$out/corpus" -max_len=1024 -timeout=60 -rss_limit_mb=4096 \
    -artifact_prefix="$out/crashes/" "$@"
