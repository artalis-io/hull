/**
 * @file hull:path
 * @module hull:path
 * @description Pure LEXICAL path-name manipulation (no filesystem authority).
 *   Lua parity: `hull.path`. Behaviourally identical to the Lua module; only the
 *   two multi-word names are camelCased per JS convention (`isAbsolute`,
 *   `isWithin`).
 * @license AGPL-3.0-or-later
 *
 * `hull:path` manipulates path NAMES lexically. `hull:fs` exercises filesystem
 * authority. These are separate on purpose.
 *
 * SECURITY BOUNDARY (do not blur it):
 *   `hull:path` performs lexical path manipulation ONLY. Lexical normalization
 *   or containment (`isWithin`) MUST NOT be used to authorize filesystem access.
 *   A lexically-contained path may still traverse a symlink that resolves
 *   OUTSIDE the root. Capability-checked filesystem operations (`hull:fs` / the
 *   capability layer) must enforce containment against the RESOLVED filesystem
 *   object, including symlink/reparse-point handling, after safe resolution.
 *   `hull:path` never touches the filesystem, never inspects symlinks, never
 *   checks existence, and grants no authority.
 *
 * Canonical separator is `/`. Paths are plain strings; there are no path
 * objects. Every operation is O(length) and deterministic.
 */

// ── internal: lexical split ────────────────────────────────────────────────
// Split `p` into { segs, absolute } with `.`/`..`/empty collapsed lexically:
//   * empty and `.` segments are dropped;
//   * `..` pops a preceding REAL segment; for an ABSOLUTE path a `..` at the
//     root is discarded (clamp: `/a/../../b` -> `/b`); for a RELATIVE path a
//     leading `..` is KEPT so it stays meaningful (`../../foo` -> `../../foo`).
function split(p) {
    const absolute = p.charAt(0) === "/";
    const segs = [];
    for (const seg of p.split("/")) {
        if (seg === "" || seg === ".") continue;
        if (seg === "..") {
            if (segs.length > 0 && segs[segs.length - 1] !== "..") {
                segs.pop();
            } else if (!absolute) {
                segs.push("..");
            }
            // absolute + nothing to pop: discard (clamp at root)
        } else {
            segs.push(seg);
        }
    }
    return { segs, absolute };
}

function build(segs, absolute) {
    const body = segs.join("/");
    if (absolute) return "/" + body;      // "/" when body === ""
    return body === "" ? "." : body;      // relative-empty -> "."
}

function checkString(v, name) {
    if (typeof v !== "string") {
        throw new TypeError("hull:path." + name + ": expected string, got " + typeof v);
    }
}

/** Lexically normalize a path (collapse `//`, `.`, `..`; never touches disk). */
function normalize(p) {
    checkString(p, "normalize");
    const { segs, absolute } = split(p);
    return build(segs, absolute);
}

/** Join components with `/` and normalize; empty components ignored. */
function join(...parts) {
    let buf = [];
    for (const c of parts) {
        checkString(c, "join");
        if (c !== "") {
            if (c.charAt(0) === "/") buf = [c];   // an absolute component RESETS the accumulator
            else buf.push(c);
        }
    }
    if (buf.length === 0) return ".";
    return normalize(buf.join("/"));
}

/** Directory portion (lexical). `foo` -> `.`, `/foo` -> `/`. */
function dirname(p) {
    checkString(p, "dirname");
    const { segs, absolute } = split(p);
    if (segs.length <= 1) return absolute ? "/" : ".";
    segs.pop();
    return build(segs, absolute);
}

/** Final component (lexical). `/` -> `/`, `.` -> `.`. */
function basename(p) {
    checkString(p, "basename");
    const { segs, absolute } = split(p);
    if (segs.length === 0) return absolute ? "/" : ".";
    return segs[segs.length - 1];
}

/** Final extension incl. the leading dot, or `""`. A dotfile (`.gitignore`) has
 *  no extension. `foo.tar.gz` -> `.gz`. Go/.NET/Java-style; no MIME inference. */
function extension(p) {
    checkString(p, "extension");
    const base = basename(p);
    let dot = -1;
    for (let i = base.length - 1; i >= 1; i--) {
        if (base.charAt(i) === ".") { dot = i; break; }
    }
    return dot < 0 ? "" : base.slice(dot);
}

/** Basename without its final extension. `archive.tar.gz` -> `archive.tar`. */
function stem(p) {
    checkString(p, "stem");
    const base = basename(p);
    const ext = extension(p);
    return ext === "" ? base : base.slice(0, base.length - ext.length);
}

/** Whether `p` is absolute (lexically: a leading `/`). NOT a security check. */
function isAbsolute(p) {
    checkString(p, "isAbsolute");
    return p.charAt(0) === "/";
}

/** Lexical relative path FROM `base` TO `target` (both normalized first, no
 *  filesystem access). Throws if incompatible (one absolute + one relative, or
 *  `base` has leading `..` not shared by `target`). */
function relative(base, target) {
    checkString(base, "relative");
    checkString(target, "relative");
    const b = split(base), t = split(target);
    if (b.absolute !== t.absolute) {
        throw new Error("hull:path.relative: cannot relate absolute and relative paths");
    }
    let i = 0;
    while (i < b.segs.length && i < t.segs.length && b.segs[i] === t.segs[i]) i++;
    for (let j = i; j < b.segs.length; j++) {
        if (b.segs[j] === "..") {
            throw new Error("hull:path.relative: base escapes above a shared root");
        }
    }
    const out = [];
    for (let j = i; j < b.segs.length; j++) out.push("..");
    for (let j = i; j < t.segs.length; j++) out.push(t.segs[j]);
    return out.length === 0 ? "." : out.join("/");
}

/** Whether `candidate` is lexically equal to or below `base` (COMPONENT-aware,
 *  after normalizing both). `isWithin("/a","/a")` -> true; `isWithin("/a",
 *  "/ab")` -> false.
 *
 *  NOT A SECURITY CHECK. Lexical containment is not filesystem authorization: a
 *  contained path may still traverse a symlink resolving outside `base`. Use for
 *  diagnostics / lexical validation only; enforce real containment in `hull:fs`
 *  against the resolved object. */
function isWithin(base, candidate) {
    checkString(base, "isWithin");
    checkString(candidate, "isWithin");
    const b = split(base), c = split(candidate);
    if (b.absolute !== c.absolute) return false;
    if (c.segs.length < b.segs.length) return false;
    for (let i = 0; i < b.segs.length; i++) {
        if (b.segs[i] !== c.segs[i]) return false;
    }
    // A candidate component immediately after the base prefix that is ".." escapes
    // ABOVE base (relative paths only, where normalize keeps leading ".."):
    // isWithin(".", "../x") / isWithin("..", "../../x"). Reject it.
    if (c.segs.length > b.segs.length && c.segs[b.segs.length] === "..") return false;
    return true;
}

const path = {
    normalize, join, dirname, basename, extension, stem, isAbsolute, relative, isWithin,
};
export { path };
