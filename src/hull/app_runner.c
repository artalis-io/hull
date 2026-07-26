/*
 * app_runner.c - the slim produced-app entry point.
 *
 * A binary from `hull build` links hl_app_run() (via templates/app_main.c)
 * instead of hull_main(). hl_app_run runs the app through hull_serve and
 * references none of the hull dev CLI: no command table, no subcommand
 * handlers, no agent introspection. Because agent/eval.c pulls both runtimes'
 * eval (it must, so `hull agent eval` works on either), keeping the CLI out of
 * a produced app is what lets a single-runtime app stay slim - it references
 * only hull_serve -> app_context -> the composed runtime.
 *
 * The hull toolchain itself keeps the full hull_main dispatcher; only produced
 * apps use this slim trampoline.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/serve.h"

int hl_app_run(int argc, char **argv)
{
    return hull_serve(argc, argv);
}

/*
 * Weak app-only stubs for toolchain-only entry points that base objects the app
 * links reference but never actually reach at runtime:
 *   - hl_lua_tool_register[_orchestration] (lua_rt_runtime.c, the tool-mode
 *     branch a sandboxed app never takes) live in mod_tool.o, which pulls the
 *     JS manifest extractor -> QuickJS.
 *   - hl_agent_api_register (serve.c, only under --agent) lives in agent_api.o,
 *     which pulls the agent introspection bodies -> both runtimes' eval.
 *
 * These stubs are defined ONLY in app_runner.o, which the hull toolchain does
 * NOT link (hull uses hull_main, not hl_app_run). So: a produced app resolves
 * these references to the no-op stubs -> mod_tool.o / agent_api.o are never
 * pulled -> the tool VM, agent, and (crucially) the OTHER interpreter all
 * dead-strip. hull links the real strong definitions directly. Params are void*
 * (the linker matches by name; pointer ABI is identical) so this TU needs no
 * runtime/agent/Keel headers. A produced app that passes --agent simply gets a
 * no-op instead of the sidecar files - acceptable for a slim app-runner.
 */
__attribute__((weak)) void hl_lua_tool_register(void *L, void *ctx)
{ (void)L; (void)ctx; }

__attribute__((weak)) void hl_lua_tool_register_orchestration(void *L)
{ (void)L; }

__attribute__((weak)) int hl_agent_api_register(void *server, void *ctx)
{ (void)server; (void)ctx; return 0; }
