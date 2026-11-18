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

(function (app, chan) {
  "use strict";

  // Writer / tree handle -> its host id, keyed by the object the plugin
  // holds, so a handle cannot be made up.
  const handleOf = new WeakMap();

  function rpc(op, args) {
    const r = chan.call(op, args);
    if (r === null || typeof r !== "object") throw refused(op, "invalid_reply");
    if (r.error !== undefined) throw refused(op, r.error);
    return r.ok === undefined ? true : r.ok;
  }

  // A write is bytes: a string becomes its UTF-8 bytes, as in Lua.
  const asBytes = (b) => (typeof b === "string" ? chan.bytes(b) : b);

  function refused(op, token) {
    const e = new Error(op + ": " + token);
    e.code = token;
    return e;
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

  function makeCtx(action) {
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

  app.transform = function transform(fn) {
    if (typeof fn !== "function") throw new TypeError("app.transform requires a function");
    app.main(async () => {
      for (;;) {
        const action = chan.next();
        if (action === null) return 0;
        let err = null;
        try {
          await fn(makeCtx(action));
        } catch (e) {
          err = e instanceof Error ? e.message : String(e);
        }
        chan.done(err);
      }
    });
  };
})
