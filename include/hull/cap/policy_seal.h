/*
 * policy_seal.h - seal the capability configs a runtime reads per call
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_POLICY_SEAL_H
#define HL_CAP_POLICY_SEAL_H

struct HlRuntime;
struct ShSealArena;

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

#endif /* HL_CAP_POLICY_SEAL_H */
