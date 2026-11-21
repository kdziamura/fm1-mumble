/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* The engine for tests/fuzz.c: app/engine.c as the device builds it, its state
 * in the section the harness puts back between inputs. */
#define PLAT_HOST 1
#ifndef FUZZ_ASAN_GLOBALS
#pragma clang section bss = "mumble_bss" data = "mumble_data"
#endif
#include "../app/engine.c"
#ifndef FUZZ_ASAN_GLOBALS
#pragma clang section bss = "" data = ""
#endif
