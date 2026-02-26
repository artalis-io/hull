# Benchmarks

All benchmarks run on a single machine using [wrk](https://github.com/wg/wrk) with 4 threads and 100 connections for 10 seconds. Results are from a MacBook Pro (Apple M4 Pro).

## Hull Results

| Endpoint | Lua (req/s) | QuickJS (req/s) | Description |
|----------|-------------|------------------|-------------|
| GET /health | 77,141 | 77,835 | JSON response, no DB |
| GET /greet/:name | 86,364 | 81,036 | Route param extraction + JSON |
| GET / | 19,277 | 19,335 | SQLite write + JSON response |

## Keel (raw HTTP server) Baseline

| Endpoint | req/s |
|----------|-------|
| GET /hello | 101,738 |

Keel is Hull's underlying HTTP server written in C with zero-copy parsing and kqueue/epoll event loops. The baseline measures raw HTTP handling with no scripting layer.

## Overhead Analysis

The scripting layer adds ~15-20% overhead for compute-only routes (no DB). The route param endpoint is faster than /health because /health includes a runtime version string lookup.

| Source | Impact |
|--------|--------|
| Lua/JS function call dispatch | ~5% |
| Request/response object creation | ~5% |
| String allocations (headers, params) | ~3-5% |
| Arena reset per request | ~1-2% |

The DB endpoint (GET /) is bottlenecked by SQLite write throughput (~19k writes/s with WAL mode), not the scripting layer. Both runtimes produce identical DB performance.

## Comparison with Other Frameworks

Approximate single-machine JSON throughput from public benchmarks (TechEmpower, community reports):

| Framework | ~req/s | Language |
|-----------|--------|----------|
| **Hull (Lua)** | **77,000-86,000** | C + Lua |
| **Hull (QuickJS)** | **78,000-81,000** | C + QuickJS |
| Fastify | ~50,000 | Node.js |
| Express | ~15,000 | Node.js |
| FastAPI | ~10,000 | Python |
| Flask | ~3,000 | Python |
| Rails | ~5,000 | Ruby |

Hull delivers Go/Rust-tier throughput from a scripting language.

## Reproducing

```bash
make                  # build hull
sh bench.sh           # run both Lua and JS benchmarks
RUNTIME=lua sh bench.sh   # Lua only
RUNTIME=js  sh bench.sh   # JS only
```

Tunable environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| THREADS | 4 | wrk thread count |
| CONNECTIONS | 100 | concurrent connections |
| DURATION | 10s | test duration |
