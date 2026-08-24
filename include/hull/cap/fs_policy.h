/*
 * fs_policy.h - compiled path-authorization policy for hull.fs (checkpoint 3).
 *
 * Checkpoint 3 of the hull.fs design (docs/hull_fs_design.md sec. 6). Separates the
 * TWO authorities the older cap layer conflated:
 *
 *   (a) ROOT CONFINEMENT - every op stays under a root. Provided entirely by the
 *       checkpoint-1/2 descriptor resolver (fs_resolve.h, virtual-root). NO
 *       manifest input.
 *   (b) PATH AUTHORIZATION - WHICH paths an app may read vs write. THIS file.
 *
 * A manifest `fs.read` / `fs.write` grant is compiled once (at cap-wiring time)
 * into an AUTHORIZATION ENTRY = a HELD anchor directory fd (ALWAYS an existing
 * directory, reached by a descriptor-bound walk from base_fd - never a
 * reconstructed host path) + a CONSTRAINT. `fs.read` and `fs.write` compile to
 * two INDEPENDENT entry sets: a read/mmap/stat/list op selects from the READ
 * set, a write op from the WRITE set (sec. 6). This object is the explicit policy;
 * the kernel sandbox stays as defense-in-depth BENEATH it. An app with no grants
 * authorizes nothing (fails closed).
 *
 * Slice A was the policy CORE: parse grants, compile grants -> entries
 * (descriptor-bound), and deterministic most-specific file selection
 * (hl_fs_policy_select). Slice B wired that into hl_cap_fs read/write/mmap. Slice C
 * adds enumeration selection (hl_fs_policy_select_list) for fs.stat / fs.list; a
 * SUBTREE lists any descendant directory, a single-terminal PATTERN exposes only
 * matching names, and EXACT/CREATE are not listable directories.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_CAP_FS_POLICY_H
#define HULL_CAP_FS_POLICY_H

#include <stddef.h>
#include "hull/cap/fs_resolve.h"   /* HlFsOpenMode, HL_FS_MAX_DEPTH */

typedef struct HlAllocator HlAllocator;   /* Hull's tracked allocator (utils/alloc.h) */

/* -- Grant glob syntax (v1, deliberately minimal) -----------------------------
 * A grant component MAY contain the wildcard '*' - "zero or more bytes WITHIN one
 * path component". '*' never crosses '/'. v1 supports ONLY '*': the metacharacters
 * '?', '[', ']', '{', '}', '\\' and the sequence "**" are REJECTED at parse with a
 * clear manifest error (unsupported in v1; no character classes, brace expansion,
 * escaping, locale, or platform-dependent matching). Matching is deterministic,
 * byte-exact, and bounded (an O(len*patlen) dynamic-programming matcher - never a
 * recursive exponential backtracker). The matcher runs on ONE component at a time
 * against a FIXED internal DP row of NAME_MAX+1 bytes: NO VLA, NO heap allocation,
 * NO recursion, and NO use of the caller's residual scratch. hl_fs_grant_parse
 * rejects any component (grant OR, at select time, caller) longer than NAME_MAX -
 * the resolver's per-component limit ("invalid_path") - so the fixed row always
 * suffices and select() allocates nothing. A wildcard grant NEVER passes its
 * wildcard text to filesystem resolution: the literal prefix before the first
 * patterned component is compiled into a held directory anchor, and the caller's
 * LITERAL bytes (not the pattern) form the residual the resolver opens.
 *
 * Security correction (documented, intentional): prior Hull versions exposed the
 * WHOLE containing directory for a wildcard grant (e.g. read of `data` slash
 * `*.csv`), because only the kernel sandbox enforced the grant (which strips the
 * glob tail to the directory) and the cap layer checked no per-path pattern. From
 * checkpoint 3 the pattern is ENFORCED: `data/a.csv` is allowed, `data/a.txt` is
 * denied (component-scoped). Apps relying on the
 * old whole-directory access must widen their manifest; that reliance was
 * unintended over-authority, not a compatibility contract. docs/api/lua.md is
 * corrected to describe the enforced semantics. */

/*
 * The four entry kinds (sec. 6 + PATTERN). The kind fixes both the anchor and the
 * per-kind symlink rule enforced later by the resolver (Slice B):
 *   - SUBTREE: anchor is the granted directory. Per the approved design (sec. 6),
 *     a SUBTREE FOLLOWS in-root symlinks with virtual-root confinement, so the
 *     anchor is obtained at COMPILE by a CONTAINED-FOLLOW resolution
 *     (HL_FS_OPEN_DIR: each component O_NOFOLLOW + a contained readlink splice), so
 *     a symlinked grant directory still loads and its target cannot escape the
 *     root. Any descendant is permitted; at RUNTIME descendant symlinks are
 *     likewise FOLLOWED, clamped within the subtree.
 *   - EXACT:   anchor is the granted file's PARENT dir; ONLY the one literal leaf
 *     name is permitted (siblings unreachable); a symlink (intermediate or
 *     terminal) is REFUSED ("symlink_denied"), never followed.
 *   - CREATE:  a not-yet-existing target; anchor is the NEAREST EXISTING ANCESTOR;
 *     ONLY the constrained missing components may be mkdirat'd and the terminal
 *     created; a symlink component is REFUSED, never followed.
 *   - PATTERN: anchor is the NEAREST EXISTING literal-prefix directory (the
 *     components before the first patterned one; the anchor may be SHALLOWER than
 *     that prefix when it is partly absent - the missing literal components are
 *     exact-constrained, created only under a WRITE, resolved not_found under a
 *     READ). The terminal component(s) are matched by the parsed patterns. The
 *     missing-literal AND pattern-constrained portions REFUSE symlinks (like
 *     EXACT/CREATE) so a matching symlink name cannot smuggle access to a
 *     non-matching target (authority follows the resolved target). Symlinks in the
 *     EXISTING literal prefix are refused during COMPILE (O_NOFOLLOW per component).
 *     A trailing-slash pattern grant is rejected at parse (no v1 descendant sense).
 *
 *     Multi-component PATTERN WRITE creation (e.g. grant `logs`+`*`+`*.txt`,
 *     caller `logs/2026/a.txt`): a WRITE may create the missing literal-prefix
 *     components AND the NONTERMINAL matched pattern components (the caller's
 *     literal `2026`, made a directory), then create/truncate the TERMINAL matched
 *     component (`a.txt`) as a file. Only the caller's LITERAL matched names are
 *     passed to mkdirat, each classify-before-open and symlink-refused (a symlink
 *     found where a patterned intermediate directory is expected fails
 *     "symlink_denied", never followed). A READ creates nothing and returns
 *     not_found when any required component is absent. (Creation is a Slice-B
 *     runtime behavior; this defines the model the entry carries.)
 */
typedef enum {
    HL_FS_ENTRY_SUBTREE = 0,
    HL_FS_ENTRY_EXACT,
    HL_FS_ENTRY_CREATE,
    HL_FS_ENTRY_PATTERN,
} HlFsEntryKind;

/* The terminal shape of a CREATE grant: exact file (only that leaf may be
 * created) vs subtree (descendants may be created). Taken from the grant's
 * `directory_intent` bit (captured pre-normalization - NOT from a trailing slash
 * after normalization, which strips it). Unused for SUBTREE/EXACT/PATTERN. */
typedef enum {
    HL_FS_TERMINAL_FILE = 0,   /* out/result.bin - only that terminal leaf */
    HL_FS_TERMINAL_SUBTREE,    /* out/ (absent)  - any descendant beneath it */
} HlFsTerminalKind;

/*
 * A PARSED grant (pure string product of hl_fs_grant_parse - NO filesystem
 * access). `comp` are the normalized path components (no "..", no trailing slash,
 * no NUL). Components in [0, first_pattern) are guaranteed LITERAL; components in
 * [first_pattern, n) may contain '*' (patterns; a literal-looking tail component
 * matches exactly). `directory_intent` records whether the RAW grant ended in a
 * trailing slash (terminal subtree) - captured BEFORE normalization so it does
 * not depend on a formatting detail that normalization would strip; it is ignored
 * when first_pattern < n (a pattern is always terminal). `comp`/its strings are
 * owned by the grant's allocator; free with hl_fs_grant_free.
 */
typedef struct {
    char        **comp;
    size_t        n;
    size_t        first_pattern;    /* index of first patterned component; == n if pure literal */
    int           directory_intent; /* raw grant had a trailing slash (terminal subtree).
                                     * REJECTED at parse when first_pattern < n: a
                                     * trailing-slash pattern grant (`data/` + `*` + `/`)
                                     * has no defined descendant semantics in v1. */
    HlAllocator  *alloc;            /* allocator that owns comp[]/strings (for _free) */
} HlFsGrant;

#define HL_FS_GRANT_INIT ((HlFsGrant){ .comp = NULL, .n = 0, .first_pattern = 0, \
                                       .directory_intent = 0, .alloc = NULL })

/*
 * One compiled authorization entry. `anchor_fd` is a HELD, always-existing
 * directory fd (reached by a descriptor-bound walk from base_fd - SUBTREE follows
 * in-root symlinks contained, EXACT/CREATE/PATTERN refuse symlinks - and
 * fstat-confirmed a directory) that the op's terminal syscall is issued relative
 * to, so selection + confinement are race-free with no separate resolved-path
 * re-check (sec. 6). `grant` is the grant's components (literal + pattern text),
 * retained for deterministic most-specific selection and pattern matching;
 * component-aware ("data" never matches "database"). Components in
 * [first_pattern, grant_n) are the parsed patterns matched against the caller's
 * terminal components. Never holds a reconstructed absolute host path.
 */
typedef struct {
    HlFsEntryKind    kind;
    int              anchor_fd;       /* held existing-directory fd (>=0 always: the
                                       * NEAREST EXISTING literal ancestor - never -1) */
    char           **grant;           /* grant components relative to base_dir (owned) */
    size_t           grant_n;
    /* The grant components split into THREE ranges by anchor_depth and
     * first_pattern (anchor_depth <= first_pattern <= grant_n):
     *   [0, anchor_depth)          reached by anchor_fd (existing dirs, held).
     *   [anchor_depth, first_pattern)  MISSING literal components - exact-constrained
     *                              dirs: a READ resolves not_found until they exist;
     *                              a WRITE may mkdirat ONLY these; symlinks refused.
     *   [first_pattern, grant_n)   PATTERN components (byte-matched, within-component,
     *                              symlinks refused). Empty (== grant_n) for non-pattern.
     * Per kind: SUBTREE anchor_depth==first_pattern==grant_n (anchor IS the dir);
     * EXACT anchor_depth==grant_n-1==first_pattern-... (parent held; grant[grant_n-1]
     * is the existing exact leaf); CREATE first_pattern==grant_n with a missing tail;
     * PATTERN first_pattern<grant_n (optionally a missing literal middle too). */
    size_t           anchor_depth;    /* existing-ancestor depth reached by anchor_fd */
    size_t           first_pattern;   /* index of first pattern component (== grant_n if none) */
    HlFsTerminalKind terminal;        /* CREATE / PATTERN terminal shape (FILE in v1) */
    /* Specificity for deterministic most-specific selection (sec. 6): primary =
     * literal_components (fully-literal, wildcard-free component count) DESC,
     * secondary = literal_bytes (non-'*' byte count) DESC. Entries are stored
     * pre-sorted by (specificity DESC, grant-text ASC) so selection scans in
     * priority order and any residual tie is broken lexicographically - total and
     * deterministic. Identical grants are deduplicated and same-component grants
     * with conflicting intent are REJECTED at compile (never order-dependent). */
    size_t           literal_components;
    size_t           literal_bytes;
} HlFsAuthEntry;

/*
 * The compiled policy: two INDEPENDENT entry sets plus the owning allocator and
 * the app-root fd. `base_fd` is retained only for compiling CREATE anchors and
 * for teardown; it is NOT a catch-all grant (no match -> "permission" denial).
 *
 * Zero-initialization is NOT a valid state (base_fd 0 is a real fd - stdin).
 * ALWAYS initialize with HL_FS_POLICY_INIT, which sets base_fd = -1 so
 * hl_fs_policy_free never closes a fd it does not own. hl_fs_policy_free is safe
 * on an HL_FS_POLICY_INIT-initialized policy and idempotent on a compiled one
 * (it -1's every fd it closes and NULLs the arrays it frees).
 */
typedef struct {
    HlAllocator    *alloc;
    int             base_fd;          /* app-root dirfd, or -1 when uninitialized/freed */
    HlFsAuthEntry  *read;             /* READ set (read/mmap/stat/list select here) */
    size_t          read_n;
    HlFsAuthEntry  *write;            /* WRITE set (write selects here) */
    size_t          write_n;
} HlFsPolicy;

#define HL_FS_POLICY_INIT ((HlFsPolicy){ .alloc = NULL, .base_fd = -1, \
                                         .read = NULL, .read_n = 0,     \
                                         .write = NULL, .write_n = 0 })

/*
 * The result of selecting an entry for a caller path. `residual` is written INTO
 * the caller-provided `scratch` (the selected entry's literal caller components
 * joined with '/', relative to entry->anchor_fd) - it is OWNED BY SCRATCH, valid
 * until scratch is reused, and is "." (never "") when the caller path IS the
 * grant root, because the resolver rejects an empty path. A NULL entry means
 * DENIED, with `err` a stable token.
 */
typedef struct {
    const HlFsAuthEntry *entry;       /* selected entry, or NULL == denied */
    const char          *residual;    /* into scratch; path relative to entry->anchor_fd */
    const char          *err;         /* when entry==NULL: "permission" / "invalid_path" */
} HlFsSelection;

/*
 * Parse ONE raw manifest grant into a HlFsGrant (pure - no filesystem access).
 * LENGTH-BEARING: `raw`/`raw_len` are the exact grant bytes, so an EMBEDDED NUL
 * ("data\0/secret") is detected and REJECTED here rather than silently truncated.
 * Captures directory_intent from a trailing slash BEFORE normalization, splits
 * into components, and locates the first patterned component. A grant that
 * normalizes to ZERO components ("." / "./" / "././") is the BASE-ROOT grant: it
 * authorizes the whole app dir and every descendant (compiled to a 0-component
 * SUBTREE at base_fd, the least-specific entry). Rejects an absolute path, any
 * ".." component, an embedded NUL, an over-deep path (> HL_FS_MAX_DEPTH
 * components), a trailing-slash PATTERN grant, and any unsupported metacharacter
 * ('?', '[', ']', '{', '}', '\\', or "**") with a stable *err ("invalid_path" /
 * "unsupported_pattern"). On success writes *out
 * (an owning value; free with hl_fs_grant_free) and returns 0; on failure returns
 * -1 with *err set, and *out is left HL_FS_GRANT_INIT (nothing to free).
 *
 * `out` MUST be HL_FS_GRANT_INIT on entry. hl_fs_grant_free is idempotent on an
 * HL_FS_GRANT_INIT grant and resets a freed grant back to HL_FS_GRANT_INIT.
 */
int hl_fs_grant_parse(const char *raw, size_t raw_len, HlAllocator *alloc,
                      HlFsGrant *out, const char **err);

void hl_fs_grant_free(HlFsGrant *grant);

/*
 * Compile parsed grants into a policy. `alloc` owns every allocation in the
 * result (entry arrays + deep-copied grant components); it is stored on the
 * policy for hl_fs_policy_free. Compilation DEEP-COPIES all grant data it needs,
 * so the caller MAY free the parsed `read`/`write` grants immediately after this
 * returns. `alloc` MUST be non-NULL (a grant list may be empty, so the allocator
 * cannot be inferred from the grants).
 *
 * `base_dir` is the app root, opened via hl_fs_open_base() - the trusted root is
 * followed ONCE (checkpoint-1 semantics), NOT O_NOFOLLOW. Grants are classified by
 * a descriptor-bound walk from base_fd, never reconstructing a host path, with
 * PER-KIND symlink semantics (design sec. 6):
 *   - SUBTREE: the grant directory is resolved with a CONTAINED-FOLLOW
 *     (HL_FS_OPEN_DIR) - in-root symlinks in the grant path are followed and
 *     clamped within the root, so a symlinked grant directory still loads (this
 *     preserves the pre-checkpoint-3 behavior; the target cannot escape the root
 *     and the held fd pins the inode).
 *   - EXACT / CREATE / PATTERN: the existing literal prefix is walked
 *     openat(O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC) + fstat-confirm a directory (a
 *     classify/open swap re-classified within a bound); ANY symlink in that path
 *     is REFUSED ("symlink_denied"), never followed - so a symlink cannot alias an
 *     exact/pattern/create target.
 * Classification:
 *   all literal comps exist, terminal is a dir      -> SUBTREE (or PATTERN if the
 *                                                      grant carries patterns)
 *   all literal comps exist, terminal is a non-dir  -> EXACT (no patterns)
 *   some literal comps missing (nearest existing
 *     ancestor is the anchor; missing tail retained -> CREATE, or PATTERN when the
 *     as exact-constrained comps)                      grant carries patterns
 *
 * Within EACH set, identical grants are DEDUPLICATED and two grants with the SAME
 * components but CONFLICTING terminal intent (`data` file vs `data/` dir) are
 * REJECTED (deterministic, not input-order-dependent). The READ and WRITE sets
 * are INDEPENDENT: a grant present in both is retained in BOTH (they express
 * different authority - readable need not be writable). Entries within a set are
 * stored pre-sorted by specificity (literal_components DESC, then literal_bytes
 * DESC, then grant-text ASC) so selection is deterministic.
 *
 * *out MUST be HL_FS_POLICY_INIT on entry. On success returns 0 (free with
 * hl_fs_policy_free). On failure returns -1 with *err set and *out left as
 * HL_FS_POLICY_INIT (no partial policy, no leaked fds).
 */
int hl_fs_policy_compile(const char *base_dir, HlAllocator *alloc,
                         const HlFsGrant *read,  size_t read_n,
                         const HlFsGrant *write, size_t write_n,
                         HlFsPolicy *out, const char **err);

/*
 * Convenience: parse RAW manifest grant strings (fs.read / fs.write) and compile
 * them into a policy in one call. Each raw string is parsed with hl_fs_grant_parse
 * (NUL-terminated, so its length is strlen) and the grants are freed before
 * returning (hl_fs_policy_compile deep-copies). A parse or compile failure returns
 * -1 with *err set and *out left HL_FS_POLICY_INIT. *out MUST be HL_FS_POLICY_INIT
 * on entry; free the result with hl_fs_policy_free.
 */
int hl_fs_policy_compile_manifest(const char *base_dir, HlAllocator *alloc,
                                  const char *const *read,  size_t read_n,
                                  const char *const *write, size_t write_n,
                                  HlFsPolicy *out, const char **err);

/*
 * Select the authorizing entry for `caller_path` (relative, no "..", no trailing
 * slash - the hl_fs_open_at lexical contract) in the set for `mode` (READ set for
 * HL_FS_OPEN_READ, WRITE set for HL_FS_OPEN_WRITE; any other mode value FAILS
 * CLOSED -> denied). Deterministic: entries are scanned in stored priority order
 * and the first match wins (most literal components, then most literal bytes,
 * then lexicographic grant text). A SUBTREE match permits any descendant; EXACT
 * requires exact component equality; CREATE permits the constrained terminal
 * (and, for a subtree terminal, descendants); PATTERN requires the caller to have
 * exactly grant_n components whose literal prefix matches exactly and whose
 * terminal components match the parsed patterns (byte-exact, within-component).
 *
 * MOST-SPECIFIC-WINS SHADOWING (deliberate, documented): a narrower grant
 * intentionally SHADOWS a broader overlapping one. With both `data/` (SUBTREE)
 * and `data` + `*.csv` (PATTERN), a caller `data/link.csv` selects the PATTERN
 * entry (higher specificity) and is therefore DENIED (`symlink_denied`, PATTERN
 * refuses symlinks) even though the SUBTREE grant alone would have followed it.
 * This is most-specific-policy-wins, NOT union-of-grants. (Selecting the most
 * specific entry that CAN authorize the RESOLVED op is deferred to a later slice;
 * v1 is pure and does not open anything to decide selection.) A dedicated test
 * locks this.
 *
 * Errors (stable tokens, all fail closed): no match -> entry == NULL,
 * err = "permission"; a lexically invalid caller path (absolute / ".." / trailing
 * slash) -> "invalid_path"; a `mode` that is neither READ nor WRITE -> "permission"
 * (denied); a `scratch` shorter than strlen(caller_path)+2 -> "invalid_args". On a
 * match, `residual` is written into `scratch` as the LITERAL caller components
 * relative to entry->anchor_fd ("." for the grant root). Pure: NO filesystem
 * access - selection never opens, stats, or reads anything.
 */
HlFsSelection hl_fs_policy_select(const HlFsPolicy *policy, const char *caller_path,
                                  HlFsOpenMode mode, char *scratch, size_t scratch_len);

/*
 * The result of selecting an entry to ENUMERATE a directory (fs.list, Slice C).
 * Distinct from hl_fs_policy_select (which selects a FILE leaf for read/write/stat):
 * listing targets a DIRECTORY, and a PATTERN grant must expose ONLY matching names.
 *
 *   entry    - selected entry, or NULL == denied (err set).
 *   residual - the directory to enumerate, relative to entry->anchor_fd, written
 *              INTO scratch ("." for the anchor itself). OWNED BY SCRATCH.
 *   filter   - NULL for a SUBTREE entry (every child returned). For a PATTERN entry,
 *              the single terminal pattern component (e.g. "*.csv") that each
 *              enumerated child NAME must byte-match (component-scoped) to be
 *              returned. Points into entry->grant (valid for the policy lifetime),
 *              never into scratch.
 *   sympol   - HL_FS_SYMLINK_FOLLOW for SUBTREE, HL_FS_SYMLINK_REFUSE for PATTERN,
 *              applied to the walk that opens `residual` as a directory.
 */
typedef struct {
    const HlFsAuthEntry *entry;
    const char          *residual;
    const char          *filter;
    HlFsSymlink          sympol;
    const char          *err;
} HlFsListSelection;

/*
 * Select the entry authorizing ENUMERATION of directory `caller_path` from the READ
 * set. Pure (NO filesystem access), deterministic, same specificity order as
 * hl_fs_policy_select. Per kind:
 *   - SUBTREE: the caller directory at/under the grant is listable; filter = NULL
 *     (all children); every descendant directory is enumerable.
 *   - EXACT / CREATE: a file (or absent) grant is NEVER a listable directory -> no
 *     match, so a caller can list neither the grant nor its parent nor its siblings.
 *   - PATTERN: GOVERNS listing of the grant's literal PREFIX directory (grant[0 ..
 *     first_pattern)) when `caller_path` equals that prefix. It is LISTABLE only for
 *     a SINGLE-TERMINAL-COMPONENT pattern (v1 restriction: first_pattern ==
 *     grant_n - 1) -> filter = grant[first_pattern]. A governing MULTI-component
 *     pattern (first_pattern < grant_n - 1, e.g. `logs`+`*`+`*.txt`) is NOT listable
 *     in v1 -> "permission": depth-aware pattern enumeration would reveal intermediate
 *     names matching an earlier '*' that contain no authorized terminal file, a
 *     metadata-exposure widening that deserves a separate design.
 *   - EXACT / CREATE: a file (or absent) grant NEVER governs a directory listing.
 *
 * MOST-SPECIFIC SHADOWING RUNS BEFORE THE LISTABILITY CHECK (deliberate, tested):
 * selection scans in stored priority order (specificity DESC, then grant-text ASC)
 * and the FIRST entry that GOVERNS `caller_path` is chosen; the per-kind listability
 * decision is then made ON THAT ENTRY. A narrower governing entry therefore SHADOWS
 * a broader overlapping one and its verdict stands - it is NOT skipped to fall
 * through to the broader grant. With both `logs/` (SUBTREE) and `logs`+`*`+`*.txt`
 * (PATTERN), `list("logs")` selects the more-specific multi-component PATTERN and
 * returns "permission"; it must NOT list via the SUBTREE. Likewise multiple
 * same-prefix terminal patterns (`data`+`*.csv` and `data`+`*.txt`) resolve to the
 * single most-specific/lexicographic winner and apply ITS filter alone - this is
 * deterministic policy selection, NEVER a union of filters. Dedicated tests lock
 * both cases. (Deeper callers where only the SUBTREE governs - e.g. `list("logs/x")`
 * - are authorized by the SUBTREE as normal; shadowing bites only at the overlap.)
 *
 * Errors (stable tokens, fail closed): no governing entry -> entry == NULL, err =
 * "permission"; a governing-but-unlistable entry -> entry == NULL, err = "permission";
 * a lexically invalid caller path (absolute / ".." / trailing slash) -> "invalid_path";
 * a `scratch` shorter than strlen(caller_path)+2 -> "invalid_args".
 */
HlFsListSelection hl_fs_policy_select_list(const HlFsPolicy *policy,
                                           const char *caller_path,
                                           char *scratch, size_t scratch_len);

/*
 * Match ONE path component `name` (nlen bytes) against a v1 grant pattern component
 * `pattern` (plen bytes; single-component '*' wildcard, byte-exact, bounded - the
 * SAME matcher compile/select use). Returns 1 on match, 0 otherwise (including any
 * component or pattern longer than NAME_MAX, or a NULL argument). fs.list uses it to
 * filter a PATTERN grant's enumeration to the matching names (via the `filter`
 * returned by hl_fs_policy_select_list).
 */
int hl_fs_pattern_match(const char *pattern, size_t plen, const char *name, size_t nlen);

/*
 * Close every owned anchor/base fd and free the entry arrays via policy->alloc.
 * Safe on an HL_FS_POLICY_INIT policy and idempotent (post-call the policy is
 * HL_FS_POLICY_INIT again). NEVER call on a {0}-initialized policy.
 */
void hl_fs_policy_free(HlFsPolicy *policy);

#endif /* HULL_CAP_FS_POLICY_H */
