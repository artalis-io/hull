// ts_node_crosscheck.mjs - M5 of the TypeScript frontend (docs/typescript_design.md section 5):
// cross-check Hull's type erasure against Node's module.stripTypeScriptTypes().
//
// Both blank type syntax to whitespace with every offset kept, so for each accepted corpus file
// Node's output must equal the committed expected .js byte for byte, apart from the closed list of
// differences below - each allowed only where Hull's output is spaces:
//   1. Node blanks a non-ASCII character with a whitespace character of the same UTF-8 length
//      (2 bytes U+00A0, 3 bytes U+FEFF, 4 bytes a space and U+FEFF); Hull writes one space per byte.
//   2. Node writes a `;` where it erases a statement, against ASI; Hull leaves the statements it
//      accepts blank and refuses the ones where ASI would join the next line (refuse/asi_*).
// Every refuse case Node also refuses must stay refused; the cases Node strips are listed in
// NODE_STRIPS (slice-1 deferrals and Hull's conservative refusals), and the list must be exact.
//
// Run by `make check-ts-node` (CI job `types`, Node 22.22.0). With no stripTypeScriptTypes (Node
// before 22.13) it skips, unless HULL_REQUIRE_NODE_STRIP=1.
//
// SPDX-License-Identifier: AGPL-3.0-or-later
import * as mod from "node:module";
import fs from "node:fs";
import path from "node:path";

const CORPUS = process.argv[2] || "tests/fixtures/ts_frontend";

// Refuse cases Node's strip mode accepts, with why Hull refuses them.
const NODE_STRIPS = new Map([
    ["abstract_class.ts", "class modifiers: slice 2"],
    ["arrow_return_line_break.ts", "line break before => after a return type: Hull refuses"],
    ["asi_array.ts", "erased statement before `[`: Hull refuses, Node inserts `;`"],
    ["asi_call.ts", "erased statement before `(`: Hull refuses, Node inserts `;`"],
    ["asi_empty_statement.ts", "erased statement before `;`: Hull refuses, Node inserts `;`"],
    ["asi_semicolon.ts", "erased statement before `;`: Hull refuses, Node inserts `;`"],
    ["asi_template.ts", "erased statement before a template: Hull refuses, Node inserts `;`"],
    ["declaration_not_in_list.ts", "type declaration outside a statement list: Hull refuses"],
    ["declare_class.ts", "declare class: slice 2"],
    ["declare_global.ts", "declare global: slice 2"],
    ["declare_initializer.ts", "initializer in a declare declaration: TypeScript rejects it"],
    ["declare_module.ts", "declare module: slice 2"],
    ["generic_arrow.ts", "generics with < speculation: slice 2"],
    ["generic_class.ts", "generics with < speculation: slice 2"],
    ["generic_function.ts", "generics with < speculation: slice 2"],
    ["implements.ts", "implements: slice 2"],
    ["import_type_require.ts", "import type x = require(): slice 2"],
    ["modifiers.ts", "class member modifiers: slice 2"],
    ["overloads.ts", "overloads: slice 2"],
    ["this_parameter.ts", "this parameter: slice 2"],
]);

const strip = mod.stripTypeScriptTypes;
if (typeof strip !== "function") {
    const msg = `ts-node-crosscheck: Node ${process.version} has no module.stripTypeScriptTypes`;
    if (process.env.HULL_REQUIRE_NODE_STRIP === "1") { console.error(`${msg} (required)`); process.exit(1); }
    console.log(`${msg}; skipped`);
    process.exit(0);
}

const SP = 0x20;
// Node's blank for one non-ASCII character of n UTF-8 bytes.
const WIDE = new Map([[2, [0xc2, 0xa0]], [3, [0xef, 0xbb, 0xbf]], [4, [0x20, 0xef, 0xbb, 0xbf]]]);
const utf8Len = (b) => (b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : b >= 0xc0 ? 2 : 1);

// Compare Node's output with Hull's under the documented differences; returns null or a message.
function compare(src, node, hull) {
    if (node.length !== hull.length) return `length ${node.length} vs ${hull.length}`;
    for (let i = 0; i < node.length;) {
        const n = utf8Len(src[i]);                 // the source character starting at i
        const wide = n > 1 && WIDE.get(n);
        if (wide && hull.subarray(i, i + n).every((b) => b === SP) && wide.every((b, k) => node[i + k] === b)) {
            i += n; continue;                       // a blanked non-ASCII character
        }
        if (node[i] === hull[i]) { i++; continue; }
        if (node[i] === 0x3b && hull[i] === SP) { i++; continue; }        // Node's `;`
        return `byte ${i}: node ${node[i].toString(16)}, hull ${hull[i].toString(16)}`;
    }
    return null;
}

let failures = 0, accepted = 0, refused = 0;
const fail = (m) => { console.error(`ts-node-crosscheck: ${m}`); failures++; };
const list = (dir) => fs.readdirSync(dir).filter((f) => f.endsWith(".ts")).sort();

for (const f of list(path.join(CORPUS, "accept"))) {
    const src = fs.readFileSync(path.join(CORPUS, "accept", f));
    const hull = fs.readFileSync(path.join(CORPUS, "accept", f.replace(/\.ts$/, ".js")));
    let node;
    try { node = Buffer.from(strip(src.toString("utf8"), { mode: "strip" }), "utf8"); }
    catch (e) { fail(`accept/${f}: Node refuses it (${e.code || e.message})`); continue; }
    const why = compare(src, node, hull);
    if (why) fail(`accept/${f}: Node's output differs from the expected .js at ${why}`);
    accepted++;
}

const seen = new Set();
for (const f of list(path.join(CORPUS, "refuse"))) {
    const src = fs.readFileSync(path.join(CORPUS, "refuse", f), "utf8");
    let stripped = true;
    try { strip(src, { mode: "strip" }); } catch { stripped = false; }
    if (stripped && !NODE_STRIPS.has(f)) fail(`refuse/${f}: Node strips it; add it to NODE_STRIPS with the reason Hull refuses`);
    if (!stripped && NODE_STRIPS.has(f)) fail(`refuse/${f}: Node refuses it; remove it from NODE_STRIPS`);
    seen.add(f);
    refused++;
}
for (const f of NODE_STRIPS.keys()) if (!seen.has(f)) fail(`NODE_STRIPS names refuse/${f}, which does not exist`);

if (accepted < 18 || refused < 29) fail(`corpus too small: ${accepted} accept, ${refused} refuse`);
if (failures) process.exit(1);
console.log(`ts-node-crosscheck: Node ${process.version}: ${accepted} accept cases match, ${refused} refuse cases agree`);
