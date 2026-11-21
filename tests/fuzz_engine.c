/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* The engine for tests/fuzz.c: app/engine.c as the device builds it, its state
 * in the section the harness puts back between inputs. */
#define PLAT_HOST 1
#pragma clang section bss = "mumble_bss" data = "mumble_data"
#include "../app/engine.c"
#pragma clang section bss = "" data = ""
