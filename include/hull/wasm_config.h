/*
 * wasm_config.h - resolve the WASM compute ceilings onto a runtime
 *
 * One three-tier resolution (CLI > manifest `wasm = {...}` > compile-time
 * defaults, each clamped to the compile-time maximum) for every entry point:
 * the Keel server (serve.c), the Keel-free app.main runner (serve_cli.c) and
 * the app context behind `hull test` / `hull agent`. Before round 6 only the
 * server applied it, so an app.main app's manifest ceilings and the operator's
 * --wasm-* flags were silently ignored (M3). The result is a CEILING: the
 * bindings clamp every per-call / per-instance option to it.
 *
 * Header-only: the base links it without the composed wasm feature.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_WASM_CONFIG_H
#define HL_WASM_CONFIG_H

#ifdef HL_ENABLE_WASM

#include "hull/limits/wasm.h"
#include "hull/manifest.h"
#include "hull/runtime.h"
#include "hull/utils/parse_size.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The operator's --wasm-* overrides; 0 = not given. */
typedef struct HlWasmCliLimits {
    long long heap;
    long long stack;
    long long gas;
    long long timeout_ms;
    long long max_input;
    long long max_output;
} HlWasmCliLimits;

/* Parse one --wasm-* option at argv[*i] (its value at argv[*i + 1]). Returns
 * 1 when it was one (and advances *i past the value), 0 when it is not a
 * --wasm-* option, -1 on a bad value (reported on stderr). */
static inline int hl_wasm_cli_flag(int argc, char **argv, int *i,
                                   HlWasmCliLimits *out)
{
    const char *a = argv[*i];
    long long *dst = NULL;
    int is_size = 0;
    if      (strcmp(a, "--wasm-heap") == 0)       { dst = &out->heap;       is_size = 1; }
    else if (strcmp(a, "--wasm-stack") == 0)      { dst = &out->stack;      is_size = 1; }
    else if (strcmp(a, "--wasm-max-input") == 0)  { dst = &out->max_input;  is_size = 1; }
    else if (strcmp(a, "--wasm-max-output") == 0) { dst = &out->max_output; is_size = 1; }
    else if (strcmp(a, "--wasm-gas") == 0)        dst = &out->gas;
    else if (strcmp(a, "--wasm-timeout-ms") == 0) dst = &out->timeout_ms;
    else return 0;
    if (*i + 1 >= argc) {
        fprintf(stderr, "hull: %s needs a value\n", a);
        return -1;
    }
    const char *v = argv[++*i];
    long long n;
    if (is_size) {
        n = hl_parse_size(v);
    } else {
        char *end;
        n = strtoll(v, &end, 10);
        if (*end != '\0') n = -1;
    }
    if (n <= 0) {
        fprintf(stderr, "hull: invalid %s: %s\n", a, v);
        return -1;
    }
    *dst = n;
    return 1;
}

/* Resolve the ceilings onto rt->wasm_config. @p m (the app's manifest) and
 * @p cli may each be NULL. Must run before the runtime's policy is sealed. */
static inline void hl_wasm_config_resolve(HlRuntime *rt, const HlManifest *m,
                                          const HlWasmCliLimits *cli)
{
    uint64_t wh = m ? m->wasm_heap : 0;
    uint64_t ws = m ? m->wasm_stack : 0;
    int64_t  wg = m && m->wasm_gas > 0 ? m->wasm_gas : 0;   /* <= 0: default */
    uint64_t wt = m ? m->wasm_timeout_ms : 0;               /* 0: default */
    uint64_t wi = m ? m->wasm_max_input : 0;
    uint64_t wo = m ? m->wasm_max_output : 0;

    /* CLI overrides manifest (operator > developer) */
    if (cli) {
        if (cli->heap > 0)       wh = (uint64_t)cli->heap;
        if (cli->stack > 0)      ws = (uint64_t)cli->stack;
        if (cli->gas > 0)        wg = (int64_t)cli->gas;
        if (cli->timeout_ms > 0) wt = (uint64_t)cli->timeout_ms;
        if (cli->max_input > 0)  wi = (uint64_t)cli->max_input;
        if (cli->max_output > 0) wo = (uint64_t)cli->max_output;
    }

    /* Clamp to compile-time maximums */
    if (wh > (uint64_t)HL_WASM_MAX_HEAP)       wh = (uint64_t)HL_WASM_MAX_HEAP;
    if (ws > (uint64_t)HL_WASM_MAX_STACK)      ws = (uint64_t)HL_WASM_MAX_STACK;
    if (wg > HL_WASM_MAX_GAS)                  wg = HL_WASM_MAX_GAS;
    if (wt > (uint64_t)HL_WASM_MAX_TIMEOUT_MS) wt = (uint64_t)HL_WASM_MAX_TIMEOUT_MS;
    if (wi > (uint64_t)HL_WASM_MAX_IO_SIZE)    wi = (uint64_t)HL_WASM_MAX_IO_SIZE;
    if (wo > (uint64_t)HL_WASM_MAX_IO_SIZE)    wo = (uint64_t)HL_WASM_MAX_IO_SIZE;

    rt->wasm_config.heap_size  = (uint32_t)wh;
    rt->wasm_config.stack_size = (uint32_t)ws;
    rt->wasm_config.gas        = wg;
    rt->wasm_config.timeout_ms = (uint32_t)wt;
    rt->wasm_config.max_input  = wi;
    rt->wasm_config.max_output = wo;
}

#endif /* HL_ENABLE_WASM */
#endif /* HL_WASM_CONFIG_H */
