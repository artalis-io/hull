// hull:_build_plugin - app.transform for a JavaScript build plugin
// (docs/buildcontext_design.md §9.1). Internal: evaluated by the runtime as a
// script when the manifest declares hull/build-plugin@1
// (runtime/js/build_plugin.c), which calls the function it evaluates to with
// the app and the channel. The twin of stdlib/lua/hull/_build_plugin.lua.
//
// Every ctx call is one request to the build host, which defines the
// operation, checks its arguments and owns its authority; this side only
// names it. A refused call throws an Error whose `code` is the host's token.
//
//   app.transform(async (ctx) => {
//     const src = ctx.inputs.read("src/a.js");     // an ArrayBuffer
//     const w = ctx.outputs.createBlob("out");
//     w.write(src); w.close();
//     ctx.outputs.declareGeneratedOutput({ name: "./a.js" }, w);
//   });
//
// Under `hull test` the transform also runs against a stub host held in
// memory (test.runTransform, §13.6): the same ctx, the same calls, answered
// from the test's spec instead of a build host. The stub is a test double:
// the host's own checks (limits, names, grants) are the real host's, which
// `hull plugins run` exercises.

(function (app, chan) {
  "use strict";

  function refused(op, token) {
    const e = new Error(op + ": " + token);
    e.code = token;
    return e;
  }

  // The ctx of one action. call(op, args) answers { ok } / { error } / {};
  // asBytes(b) is what a write sends.
  function makeCtx(action, call, asBytes) {
    // Writer / tree handle -> its host id, keyed by the object the plugin
    // holds, so a handle cannot be made up.
    const handleOf = new WeakMap();

    function rpc(op, args) {
      const r = call(op, args);
      if (r === null || typeof r !== "object") throw refused(op, "invalid_reply");
      if (r.error !== undefined) throw refused(op, r.error);
      return r.ok === undefined ? true : r.ok;
    }

    function writer(kind, id) {
      const w = kind === "blob"
        ? {
            write(bytes) { return rpc("blob.write", { handle: id, bytes: asBytes(bytes) }); },
            close() { return rpc("blob.close", { handle: id }); },
          }
        : {
            write(path, bytes, mode) {
              return rpc("tree.write", { handle: id, path, bytes: asBytes(bytes), mode });
            },
            close() { return rpc("tree.close", { handle: id }); },
          };
      handleOf.set(w, id);
      return w;
    }

    return {
      meta: action.meta,
      config: action.config,
      inputs: {
        read: (path) => rpc("inputs.read", { path }),
        stat: (path, opts) => rpc("inputs.stat", { path, opts }),
        list: (path) => rpc("inputs.list", { path }),
      },
      outputs: {
        createBlob: (name) => writer("blob", rpc("outputs.create_blob", { name })),
        createTree: (name) => writer("tree", rpc("outputs.create_tree", { name })),
        declareGeneratedOutput: (descriptor, w) =>
          rpc("outputs.declare_generated_output", { descriptor, handle: handleOf.get(w) }),
        declareArtifact: (descriptor, t) =>
          rpc("outputs.declare_artifact", { descriptor, handle: handleOf.get(t) }),
      },
      diagnostics: {
        emit: (diagnostic) => rpc("diagnostics.emit", { diagnostic }),
      },
      source: {
        units: () => rpc("source.units"),
        erasedSource: (unit) => rpc("source.erased_source", { unit }),
      },
    };
  }

  // A write is bytes: a string becomes its UTF-8 bytes, as in Lua.
  const asBytes = (b) => (typeof b === "string" ? chan.bytes(b) : b);

  // One ArrayBuffer of the chunks (each an ArrayBuffer or a typed array).
  function concat(chunks) {
    const views = chunks.map((c) => (c instanceof ArrayBuffer
      ? new Uint8Array(c) : new Uint8Array(c.buffer, c.byteOffset, c.byteLength)));
    let n = 0;
    for (const v of views) n += v.length;
    const out = new Uint8Array(n);
    let at = 0;
    for (const v of views) { out.set(v, at); at += v.length; }
    return out.buffer;
  }

  // The stub host: spec.inputs (path -> string or bytes), spec.units,
  // spec.erased (unit -> string or bytes). It records what the transform
  // writes, declares and emits; written text is kept as text too.
  function stubHost(spec) {
    const inputs = spec.inputs && typeof spec.inputs === "object" ? spec.inputs : {};
    const has = (p) => typeof p === "string" && Object.prototype.hasOwnProperty.call(inputs, p);
    const handles = new Map();
    let nextId = 0;
    const outputs = [];
    const diagnostics = [];

    const isDir = (path) => path === "." ||
      Object.keys(inputs).some((p) => p.startsWith(path + "/"));

    const ops = {
      "inputs.read": (a) => {
        if (typeof a.path !== "string") return [undefined, "invalid_args"];
        if (!has(a.path)) return [undefined, isDir(a.path) ? "not_a_regular_file" : "not_found"];
        return [asBytes(inputs[a.path])];
      },
      "inputs.stat": (a) => {
        if (typeof a.path !== "string") return [undefined, "invalid_args"];
        const o = a.opts && typeof a.opts === "object" ? a.opts : {};
        if (has(a.path)) {
          const r = { type: "file" };
          if (o.size) r.size = asBytes(inputs[a.path]).byteLength;
          if (o.exec) r.exec = false;
          return [r];
        }
        return [{ type: isDir(a.path) ? "dir" : "absent" }];
      },
      "inputs.list": (a) => {
        if (typeof a.path !== "string") return [undefined, "invalid_args"];
        if (!isDir(a.path)) return [undefined, has(a.path) ? "not_a_directory" : "not_found"];
        const prefix = a.path === "." ? "" : a.path + "/";
        const seen = new Map();
        for (const p of Object.keys(inputs)) {
          if (!p.startsWith(prefix)) continue;
          const rest = p.slice(prefix.length);
          const slash = rest.indexOf("/");
          const name = slash < 0 ? rest : rest.slice(0, slash);
          if (name && !seen.has(name)) seen.set(name, slash < 0 ? "file" : "dir");
        }
        const out = [...seen].map(([name, type]) => ({ name, type }));
        out.sort((x, y) => (x.name < y.name ? -1 : x.name > y.name ? 1 : 0));
        return [out];
      },
      "blob.write": (a) => write(a, "blob", (h) => h.chunks.push(a.bytes)),
      "tree.write": (a) => write(a, "tree",
        (h) => h.entries.push({ path: a.path, bytes: a.bytes, mode: a.mode || "regular" })),
      "blob.close": (a) => write(a, "blob", (h) => { h.open = false; }),
      "tree.close": (a) => write(a, "tree", (h) => { h.open = false; }),
      "outputs.declare_generated_output": (a) => declare(a, "generated"),
      "outputs.declare_artifact": (a) => declare(a, "artifact"),
      "diagnostics.emit": (a) => {
        if (!a.diagnostic || typeof a.diagnostic !== "object") return [undefined, "invalid_diagnostic"];
        diagnostics.push(a.diagnostic);
        return [];
      },
      "source.units": () => [Array.isArray(spec.units) ? spec.units : []],
      "source.erased_source": (a) => {
        const e = spec.erased && typeof spec.erased === "object" ? spec.erased : {};
        if (!Object.prototype.hasOwnProperty.call(e, a.unit)) return [undefined, "unsupported_op"];
        return [asBytes(e[a.unit])];
      },
    };
    for (const kind of ["blob", "tree"]) {
      ops["outputs.create_" + kind] = (a) => {
        if (typeof a.name !== "string") return [undefined, "invalid_args"];
        nextId += 1;
        handles.set(nextId, { kind, name: a.name, chunks: [], entries: [], open: true });
        return [nextId];
      };
    }
    function write(a, kind, fn) {
      const h = handles.get(a.handle);
      if (!h || h.kind !== kind) return [undefined, "invalid_handle"];
      if (!h.open) return [undefined, "closed"];
      fn(h);
      return [];
    }
    function declare(a, declared) {
      const h = handles.get(a.handle);
      if (!h) return [undefined, "invalid_handle"];
      if (h.open) return [undefined, "open_writer"];
      const o = { declared, descriptor: a.descriptor, name: h.name };
      if (h.kind === "blob") {
        o.bytes = concat(h.chunks.map(asBytes));
        if (h.chunks.every((c) => typeof c === "string")) o.text = h.chunks.join("");
      } else {
        o.entries = h.entries.map((e) => {
          const r = { path: e.path, mode: e.mode, bytes: concat([asBytes(e.bytes)]) };
          if (typeof e.bytes === "string") r.text = e.bytes;
          return r;
        });
      }
      outputs.push(o);
      return [];
    }

    function call(op, args) {
      const fn = Object.prototype.hasOwnProperty.call(ops, op) ? ops[op] : null;
      if (!fn) return { error: "unknown_op" };
      const [v, e] = fn(args || {});
      if (e !== undefined) return { error: e };
      return v === undefined ? {} : { ok: v };
    }
    return { call, outputs, diagnostics };
  }

  let transform = null;

  app.transform = function transform_(fn) {
    if (typeof fn !== "function") throw new TypeError("app.transform requires a function");
    transform = fn;
    app.main(async () => {
      for (;;) {
        const action = chan.next();
        if (action === null) return 0;
        let err = null;
        try {
          await fn(makeCtx(action, chan.call, asBytes));
        } catch (e) {
          err = e instanceof Error ? e.message : String(e);
        }
        chan.done(err);
      }
    });
  };

  // test.runTransform(spec): the registered transform against the stub host.
  // Resolves to { outputs, diagnostics, error? }.
  return async function runStub(spec) {
    if (!transform) throw new Error("test.runTransform: the app registered no app.transform");
    const s = spec && typeof spec === "object" ? spec : {};
    const host = stubHost(s);
    const action = { config: s.config || {}, meta: s.meta || {} };
    let error;
    try {
      await transform(makeCtx(action, host.call, (b) => b));
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    }
    const r = { outputs: host.outputs, diagnostics: host.diagnostics };
    if (error !== undefined) r.error = error;
    return r;
  };
})
