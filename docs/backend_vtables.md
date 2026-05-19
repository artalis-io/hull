# Async & Net Backend Vtables — Design

Replacing direct Keel coupling with two pluggable vtables — `HlAsyncBackend`
for the lower-level async/event-loop layer, `HlNetBackend` for the
HTTP/WebSocket server layer. Mirrors the `HlDbBackend` pattern that
already lets `db.*` consume SQLite (today) or Postgres (later) through one
interface.

This document is the design plan. Status: not yet implemented.

## Why this and not "replace primitives"

Phase 3d was originally scoped as "replace Keel async primitives in CLI
builds with a poll-based mini-loop". That's a one-shot fix for a single
need. The vtable framing solves the same need *and* opens three other
doors we already know we want:

1. **CLI builds drop Keel entirely** — `HlAsyncBackend` = poll backend
   gives `make HL_ENABLE_HTTP=0` a 1MB+ binary win and removes Keel
   from the link.
2. **Async-in-app.main on CLI builds** — today's `serve_cli.c` can't
   run `hull.sleep` / `compute.async` / `gpu.async` / `db.async` /
   `http.fetch` because no event loop drives them. A poll backend
   provides that loop.
3. **Future HTTP server alternatives** — `HlNetBackend` slot lets us
   eventually swap Keel for libuv, a sandboxed mini-server, an
   io_uring-tuned server, etc. — without touching the runtime layer.
4. **Better testability** — backends can be mocked. Test fixtures
   become trivial.

Cost: bigger upfront refactor (~1-2 weeks across two passes vs ~3 days
for a one-shot CLI poll loop). Pays back forever after.

## What today's coupling looks like

99 unique `kl_*` symbols and 43 unique `Kl*` types are referenced across
58 source files. They cluster into:

| Cluster | Keel symbols | Hull consumers |
|---------|--------------|----------------|
| Allocator | `KlAllocator`, `kl_allocator_default`, `kl_alloc`, `kl_realloc`, `kl_free` | All over (passed to most kl_* APIs) |
| Async core | `KlAsyncOp`, `kl_async_suspend`, `kl_async_complete` | `runtime/{lua,js}/async.c`, `cap/http_async.c`, `cap/wasm.c` (async), `worker_db.c`, `worker_wasm.c`, `worker_gpu.c`, `cap/ws.c`, hull `sleep` |
| Event loop | `KlWatcher`, `KlEventCtx`, `kl_monotonic_ms` | `serve.c`, `worker_*.c`, `agent_api.c` |
| Timers | `kl_timer_add`, `kl_timer_cancel` | `runtime/{lua,js}/timers.c`, `runtime/{lua,js}/async.c` (sleep) |
| Thread pool | `KlThreadPool`, `KlThreadPoolConfig`, `KlWorkItem`, `kl_thread_pool_*` | `serve.c`, `worker_*.c`, `runtime/{lua,js}/worker.c` |
| HTTP server | `KlServer`, `KlConfig`, `kl_server_*`, `KlConn` | `serve.c`, `runtime/{lua,js}/dispatch.c`, `routes.c`, `agent_api.c` |
| Router | `KlRouter`, `KlRoute`, `kl_router_*` | `routes.c` (both runtimes), `runtime/{lua,js}/dispatch.c` |
| Request / response | `KlRequest`, `KlResponse`, `kl_request_*`, `kl_response_*`, `KlBodyReader`, `KlBufReader` | `runtime/{lua,js}/bindings.c`, `dispatch.c`, `cap/body.c` |
| HTTP client | `KlClient`, `KlClientConfig`, `kl_client_*`, `KlClientPool`, `kl_cpool_*`, `KlRedirectClient` | `cap/http.c`, `cap/http_async.c`, `cmd_update.c` |
| WebSocket | `KlWsServerConfig`, `kl_ws_server_*`, `KlWsClientCallbacks`, `kl_ws_client_*`, `kl_ws` | `cap/ws.c`, `runtime/{lua,js}/ws.c`, `runtime/{lua,js}/mod_ws.c` |
| SSE | `KlSse`, `kl_sse_*` | `runtime/{lua,js}/sse.c`, `runtime/{lua,js}/mod_sse.c` |
| TLS | `KlTls`, `KlTlsConfig`, `KlTlsCtx`, `KlTlsFactory`, `KlTlsResult`, `kl_tls_mbedtls_*` | `serve.c`, `cap/http.c`, `cap/smtp.c`, `cmd_update.c` |
| Compression | `KlCompressConfig`, `KlCompressCtx`, `KlCompressFactory`, `KlDecompressConfig`, `kl_compress_miniz_*`, `kl_decompress_miniz_*` | `serve.c`, `cap/http.c` |
| Static helpers | `kl_url_parse`, `kl_strerror`, `KlError` | `cap/http*.c`, `cmd_update.c`, etc. |
| Middleware | `KlCorsConfig`, `kl_cors_*` | `serve.c` |

That's a lot, but it splits cleanly into two layers:

**Async core** (event loop, watchers, timers, thread pool, async-op
suspension, allocator, monotonic time): used by every async capability
regardless of HTTP. ~12 symbols, 5 types.

**HTTP layer** (server, router, request/response, body reader, HTTP
client, WebSocket, SSE, TLS, compression, redirect, CORS, URL parse,
errors): used only when serving or making HTTP requests. ~85 symbols,
35 types.

## Two vtables

### HlAsyncBackend — async core

```c
typedef struct HlAsyncBackend {
    const char *name;                          /* "keel", "poll", ... */

    /* Event loop lifecycle */
    int    (*init)(void **ctx, HlAllocator *alloc);
    void   (*free)(void *ctx);

    /* Drive the loop. block=1 → wait for events; block=0 → poll once.
     * Returns 0 on clean shutdown, -1 on fatal error. */
    int    (*run_once)(void *ctx, int timeout_ms);
    int    (*run_until)(void *ctx, int (*should_stop)(void *), void *user);
    void   (*stop)(void *ctx);

    /* Monotonic time */
    uint64_t (*monotonic_ms)(void);

    /* Timers — fire `cb(user)` after `ms`. Returns opaque handle for
     * cancel; 0 on alloc failure. */
    uint64_t (*timer_add)(void *ctx, uint64_t ms,
                          void (*cb)(void *user), void *user);
    void   (*timer_cancel)(void *ctx, uint64_t handle);

    /* FD watcher — fires when fd is ready for read/write. */
    int    (*watcher_add)(void *ctx, int fd, int mask,
                          void (*cb)(int fd, int ready, void *user),
                          void *user);
    int    (*watcher_mod)(void *ctx, int fd, int mask);
    void   (*watcher_del)(void *ctx, int fd);

    /* Thread pool — offload blocking work to a worker; done_fn runs
     * back on the event-loop thread. */
    int    (*pool_create)(void **pool, void *ctx, int num_workers,
                          int queue_capacity);
    void   (*pool_free)(void *pool);
    int    (*pool_submit)(void *pool,
                          void (*work_fn)(void *user),
                          void (*done_fn)(void *user),
                          void (*cancel_fn)(void *user),
                          void *user);

    /* Detached async op — runtime-agnostic suspension point.
     * Hull's HlAsyncCtx already wraps this; we just route it through
     * the vtable so Keel and poll backends both fit.
     *
     * suspend(): start tracking an in-flight op. The backend doesn't
     *   care what it is — it only owns the on_resume/on_cancel/on_deadline
     *   callbacks attached to it.
     * complete(): the runtime signals the op is done; backend schedules
     *   on_resume to fire on the event-loop thread.
     *
     * (For HTTP backends this maps to Keel's connection-suspend.
     * For poll backend it's "just track this op in a list".) */
    int    (*op_suspend)(void *ctx, HlAsyncOpHandle *op);
    void   (*op_complete)(void *ctx, HlAsyncOpHandle *op);
} HlAsyncBackend;
```

### HlNetBackend — HTTP/WebSocket server

```c
typedef struct HlNetBackend {
    const char *name;                          /* "keel", ... */
    HlAsyncBackend *async;                     /* required dep */

    /* Server lifecycle */
    int    (*server_init)(void **server, HlNetServerConfig *cfg);
    void   (*server_free)(void *server);
    int    (*server_run)(void *server);        /* blocks */
    void   (*server_stop)(void *server);

    /* Routing */
    int    (*route_add)(void *server, const char *method, const char *pattern,
                        HlRouteHandler handler, void *user);
    int    (*middleware_pre)(void *server, const char *method,
                             const char *pattern,
                             HlMiddleware mw, void *user);
    int    (*middleware_post)(void *server, const char *method,
                              const char *pattern,
                              HlMiddleware mw, void *user);

    /* WebSocket endpoint registration */
    int    (*ws_endpoint)(void *server, const char *pattern,
                          HlWsConfig *cfg);

    /* Request accessors (called by handlers) */
    const char *(*req_method)(HlReqHandle *req);
    const char *(*req_path)(HlReqHandle *req);
    const char *(*req_query)(HlReqHandle *req);
    const char *(*req_header)(HlReqHandle *req, const char *name);
    size_t      (*req_body)(HlReqHandle *req, const char **data);

    /* Response builders (called by handlers) */
    void   (*res_status)(HlResHandle *res, int status);
    void   (*res_header)(HlResHandle *res, const char *name,
                         const char *value);
    void   (*res_body)(HlResHandle *res, const char *data, size_t len);
    int    (*res_file)(HlResHandle *res, const char *path);
    int    (*res_stream_begin)(HlResHandle *res);
    int    (*res_stream_write)(HlResHandle *res, const char *data,
                                size_t len);
    void   (*res_stream_end)(HlResHandle *res);

    /* SSE helpers */
    int    (*sse_begin)(HlResHandle *res);
    int    (*sse_event)(HlResHandle *res, const char *name,
                        const char *data);
    void   (*sse_end)(HlResHandle *res);

    /* Compression hook (optional — backend may bake it in) */
    int    (*body_compress)(HlResHandle *res, int level);

    /* TLS config (optional) */
    int    (*tls_set)(void *server, const char *cert_path,
                      const char *key_path);

    /* Stats */
    void   (*stats)(void *server, HlNetServerStats *out);
} HlNetBackend;
```

HTTP client is split into its own slot (some backends might want to
share TCP with the server; others won't):

```c
typedef struct HlHttpClientBackend {
    const char *name;
    HlAsyncBackend *async;

    int    (*request_sync)(HlHttpRequest *req, HlHttpResponse *out);
    int    (*request_async)(HlHttpRequest *req,
                            void (*done)(HlHttpResponse *res, void *user),
                            void *user);

    /* Connection pool */
    int    (*pool_init)(void **pool, HlHttpPoolConfig *cfg);
    void   (*pool_free)(void *pool);
} HlHttpClientBackend;
```

WebSocket client + redirect-following client follow similar shape but
are kept in their own slots for clarity.

## Why three vtables, not one

Could be one fat `HlNetBackend` containing everything. Three reasons to
split:

1. **CLI builds use async, not server.** A merged vtable forces CLI
   builds to either implement no-op server methods (gross) or carry
   conditional compilation through every method (defeats the point).
2. **Different backends may want different layers.** libuv is async-only;
   Apache modules are HTTP-only-on-top-of-OS; Keel does both. The
   right granularity matches the real shapes of backends.
3. **Test mocks become trivial.** Mock async without mocking HTTP,
   mock HTTP client without mocking server.

## Backends to ship

| Backend | Implements | Used by | Status |
|---------|------------|---------|--------|
| `keel` | `HlAsyncBackend`, `HlNetBackend`, `HlHttpClientBackend` | `HL_ENABLE_HTTP=1` (default) | New impl that wraps current Keel calls |
| `poll` | `HlAsyncBackend` | `HL_ENABLE_HTTP=0` (CLI builds) | New minimal impl: `poll(2)` event loop, `pthread`-based pool, software timer min-heap |

Future:
- `libuv` — cross-platform async, replaces both keel async and poll
- `io_uring` — Linux high-perf async
- A future sandboxed mini-server for `HlNetBackend` that's smaller
  than Keel for embedded/edge use

## How Hull consumes the vtables

Backends are wired once at startup via a single getter:

```c
const HlAsyncBackend *hl_async_backend(void);  /* compiled-in default */
const HlNetBackend   *hl_net_backend(void);    /* NULL if HL_ENABLE_HTTP=0 */
const HlHttpClientBackend *hl_http_client_backend(void);
```

Implementation:

```c
#if defined(HL_ENABLE_HTTP)
extern const HlAsyncBackend hl_async_backend_keel;
const HlAsyncBackend *hl_async_backend(void) { return &hl_async_backend_keel; }
#elif defined(HL_ASYNC_BACKEND_POLL)
extern const HlAsyncBackend hl_async_backend_poll;
const HlAsyncBackend *hl_async_backend(void) { return &hl_async_backend_poll; }
#endif
```

The `HlServerState` / `HlAsyncCtx` / etc. structs hold opaque `void *`
backend contexts instead of typed Keel structs. Hull code accesses them
only through the vtable:

```c
backend->op_suspend(ctx->backend_ctx, &ctx->op);
```

instead of today's:

```c
kl_async_suspend(server, conn, &ctx->op);
```

## Migration phases

### Phase 3d-1 — define the vtables, leave Keel in place
- Author `include/hull/async_backend.h` and `include/hull/net_backend.h`
- Define opaque handle types (`HlAsyncOpHandle`, `HlReqHandle`, etc.)
- No consumers yet; everything still uses `kl_*` directly
- Outcome: API surface scoped. No behavior change. ~2 days.

### Phase 3d-2 — write Keel-backed implementations
- `src/hull/net/async_keel.c` — wraps Keel calls behind `HlAsyncBackend`
- `src/hull/net/net_keel.c` — wraps Keel server behind `HlNetBackend`
- `src/hull/net/http_client_keel.c` — wraps `KlClient` behind `HlHttpClientBackend`
- Unit-tested in isolation
- Outcome: backends exist; Hull still uses `kl_*` directly. ~3 days.

### Phase 3d-3 — migrate consumers (mechanical)
- Replace direct `kl_*` calls in Hull's consumers with vtable calls
- One layer at a time: async helpers first, then cap/http, then worker_*,
  then runtime dispatch, then serve.c
- After each layer: run `make test + make e2e-examples + make e2e-cli`
- Heaviest layer: `runtime/{lua,js}/dispatch.c` + `bindings.c` (request /
  response handling)
- Outcome: Hull never imports a `kl_*` symbol directly. Keel becomes a
  link-time dependency only. ~4 days.

### Phase 3d-4 — write the poll backend
- `src/hull/net/async_poll.c` — minimal `poll(2)` + `pthread` impl
- ~600-800 lines: event-loop tick, timer min-heap, FD watcher table,
  thread pool (existing Keel pool design ports cleanly)
- Tested by running every `make e2e-cli` example on a HL_ENABLE_HTTP=0
  build that uses the poll backend
- Outcome: async-in-app.main works on CLI builds. ~3 days.

### Phase 3d-5 — drop Keel from `HL_ENABLE_HTTP=0` link
- Makefile: when `HL_ENABLE_HTTP=0`, the keel backend objects + `libkeel.a`
  drop out, the poll backend objects come in
- `serve_cli.c` switches from `server=NULL` to passing the poll backend's
  context — main.c's run_main now actually has an event loop
- Binary-size verification: target ≥ 1 MB drop vs HTTP=1 build
- Outcome: pure CLI Hull with full async support, no Keel in the link.
  ~2 days.

Total: ~14 days of focused work, spread across 5 commits. Each commit
keeps both build flavors green.

## What stays out of scope

- **Replacing Keel for HTTP=1 builds.** Phase 3d adds a Keel-backed
  vtable impl; it doesn't replace Keel. Default builds continue to
  link Keel and behave identically.
- **Changing app-facing API.** `app.get/post/use/ws/sse/every/daily`,
  `compute.async`, `http.fetch`, `hull.sleep`, etc. are unchanged.
  This is a backend-layer refactor only.
- **Cross-platform Windows support.** The poll backend uses POSIX
  `poll(2)`; Windows is out of scope for v1 of the backend interface.
  A future libuv backend would cover Windows.
- **Pluggable TLS backends.** Hull already has the `KlTls` vtable;
  this design doesn't subsume it. TLS stays a separate vtable that
  the net backend consumes.

## Risks

- **Interface design lock-in.** If the vtables don't capture the right
  abstractions, future backends contort to fit. Mitigation: model the
  vtables on actual Keel usage, not on what an idealized backend would
  expose. Phase 3d-1 should commit only after at least one
  semi-complete walk-through of how each existing cap would use them.

- **Async-op suspension is the trickiest interface.** Keel's
  `KlAsyncOp` is tied to a connection (resumes when the deadline fires
  or `kl_async_complete` is called by another thread). The poll
  backend has no connection — it just needs "track this op, fire its
  resume callback on the event loop thread when complete". Both fit
  the same `op_suspend`/`op_complete` shape, but the implementation
  details differ. Spend extra time on the vtable signature here.

- **Performance regression.** Every `kl_*` call gains a vtable
  indirection. For request dispatch (hot path), this is ~6 extra
  indirect calls per request — measurable in microbenchmarks but
  unlikely to be material vs network/database costs. Worth a
  before/after bench run during phase 3d-3.

- **Test coverage gap.** Hull's existing tests exercise behavior end-to-end.
  Backend-swap tests need to compare: same app → same observed behavior
  on Keel vs poll. Easiest is to add a parameterized e2e mode that runs
  the same e2e against both backends.

- **Two-phase build (3d-3 then 3d-4) leaves a window where the only
  shipping backend is Keel.** This is fine — every commit keeps the
  default build green. The poll backend is additive at the end.

## Open questions for implementation

1. Should `HlAsyncBackend.op_suspend` take an `HlAsyncOp` by-value
   (POD copy) or by-pointer (caller owns)? Keel uses by-pointer
   (caller embeds `KlAsyncOp` in their own struct via container_of).
   By-pointer matches the current usage; by-value is more vtable-friendly
   if the backend wants its own storage. Lean: by-pointer for migration
   simplicity.
2. Where do TLS contexts live? In `HlNetBackend` (each net backend owns
   its TLS) or as a separate `HlTlsBackend` vtable? Hull already
   wraps Keel's `KlTls`; the existing pattern argues for a separate
   slot but keeping things together makes the keel-net backend simpler.
3. Should `HlNetBackend` define its own request/response types, or
   reuse Hull's existing accessor shape (which today is a `KlRequest *`
   wrapped in runtime bindings)? Defining new types is cleaner but
   doubles the migration work (every caller of `kl_request_header`
   becomes a vtable call).

These need answers in Phase 3d-1 before the vtable headers ship.
