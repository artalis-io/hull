# Benchmarks

All benchmarks run on a single machine using [wrk](https://github.com/wg/wrk) with 4 threads and 100 connections for 10 seconds. Results are from a MacBook Pro (Apple M4 Pro).

## Hull Results

| Endpoint | Lua (req/s) | QuickJS (req/s) | Description |
|----------|-------------|------------------|-------------|
| GET /health | 69,884 | 72,483 | JSON response, no DB |
| GET /greet/:name | 81,863 | 76,204 | Route param extraction + JSON |
| POST /echo | 69,379 | 72,025 | Body parsing + JSON echo |
| POST /greet/:name | 63,688 | 67,450 | Route param + body parsing |
| GET / | 19,873 | 20,233 | SQLite write + JSON response |
| GET /visits | 11,508 | 22,037 | SQLite read (SELECT LIMIT 20) |

## Keel (raw HTTP server) Baseline

| Endpoint | req/s |
|----------|-------|
| GET /hello | 101,738 |

Keel is Hull's underlying HTTP server written in C with zero-copy parsing and kqueue/epoll event loops. The baseline measures raw HTTP handling with no scripting layer.

## Overhead Analysis

The scripting layer adds ~15-30% overhead for compute-only routes (no DB). The route param endpoint is faster than /health because /health includes a runtime version string lookup.

POST requests with body parsing add ~5-10% overhead vs equivalent GET routes due to body reader allocation and JSON deserialization.

| Source | Impact |
|--------|--------|
| Lua/JS function call dispatch | ~5% |
| Request/response object creation | ~5% |
| String allocations (headers, params) | ~3-5% |
| Body reader + JSON deserialization | ~5-10% |
| Arena reset per request | ~1-2% |

The DB write endpoint (GET /) is bottlenecked by SQLite write throughput (~20k writes/s with WAL mode), not the scripting layer. Both runtimes produce identical DB write performance.

The DB read endpoint (GET /visits) shows divergent performance: QuickJS (22k req/s) significantly outperforms Lua (11.5k req/s) on SELECT queries returning multiple rows, likely due to differences in result set serialization overhead.

## Comparison with Other Frameworks

Approximate single-machine JSON throughput from public benchmarks (TechEmpower, community reports):

| Framework | ~req/s | Language |
|-----------|--------|----------|
| **Hull (Lua)** | **70,000-82,000** | C + Lua |
| **Hull (QuickJS)** | **72,000-76,000** | C + QuickJS |
| Fastify | ~50,000 | Node.js |
| Express | ~15,000 | Node.js |
| FastAPI | ~10,000 | Python |
| Rails | ~5,000 | Ruby |
| Flask | ~3,000 | Python |

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
