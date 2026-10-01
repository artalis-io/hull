/*
 * policy_seal.h - seal the capability configs a runtime reads per call
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_POLICY_SEAL_H
#define HL_CAP_POLICY_SEAL_H

#include <stddef.h>

struct HlRuntime;
struct ShSealArena;

/* ── Policy spans ─────────────────────────────────────────────────────
 *
 * Some policy has to live INSIDE an object that is otherwise mutable: the
 * runtime (which also carries per-request state), the db registry (its
 * connection cache), the GPU context (pipeline caches). A pointer from such
 * an object to sealed policy is itself writable, so it was the residual
 * "root pointer" an arbitrary write could repoint.
 *
 * The pattern: the object's policy fields come FIRST, padded to
 * HL_POLICY_SPAN bytes (a union with a byte array of that size), and the
 * object is allocated page-aligned with hl_policy_page_alloc. Sealing then
 * makes exactly that leading span read-only and leaves the rest writable.
 * 64 KiB is a whole number of pages on every host Hull targets (4 KiB x86,
 * 16 KiB Apple silicon, up to 64 KiB arm64 Linux); only the pages actually
 * touched cost memory. Objects not allocated this way (tests, the tool VM)
 * work unchanged and are simply never sealed. */
#define HL_POLICY_SPAN 65536u

/** Zeroed, page-aligned memory for an object with a leading policy span. */
void *hl_policy_page_alloc(size_t size);
/** Free it (unsealing first if needed). NULL is a no-op. */
void  hl_policy_page_free(void *p, size_t size);
/** Make the leading HL_POLICY_SPAN bytes of @p p read-only. -1 if @p p is not
 *  page-aligned or the protection could not be applied. */
int   hl_policy_page_seal(void *p);
/** Make them writable again (before teardown writes or a free). */
int   hl_policy_page_unseal(void *p);

/**
 * @brief Move the cap configs @p rt points at into a sealed arena.
 *
 * The manifest's strings and arrays are sealed separately; these are the
 * structs that POINT at them - the fs config and its compiled policy, the
 * env allowlist, the http and smtp host allowlists - which the cap layer
 * reads on every call. Left in writable memory, a heap write repointed or
 * re-counted an allowlist without touching a sealed byte ("add a C2 host"
 * by bumping a count). Each config @p rt has wired is copied into @p arena
 * (initialised here), the arena is sealed, and @p rt is repointed at the
 * copies. The originals stay where they were; nothing reads them after this,
 * and the fs policy original still owns the descriptors the copy aliases.
 *
 * Call once wiring is complete: nothing may write a config afterwards.
 *
 * @return 0, or -1 if the arena could not be mapped, filled or sealed (the
 *         caller treats that as fatal; @p rt is unchanged and the arena is
 *         destroyed).
 */
int hl_policy_seal_cap_configs(struct HlRuntime *rt, struct ShSealArena *arena);

/**
 * @brief Seal the runtime's policy span (its cap-config pointers, module set,
 * kv / ssh policy, CSP, WASM limits, GPU context pointer).
 *
 * The last step of wiring, after hl_policy_seal_cap_configs: from here the
 * runtime's ROOT pointers to sealed policy cannot be repointed either. The
 * runtime must come from a factory (page-aligned); -1 otherwise, or if the
 * protection fails - fatal for the caller. hl_policy_unseal_runtime reverses
 * it for teardown (the factory's destroy also does).
 */
int hl_policy_seal_runtime(struct HlRuntime *rt);
int hl_policy_unseal_runtime(struct HlRuntime *rt);

#endif /* HL_CAP_POLICY_SEAL_H */
