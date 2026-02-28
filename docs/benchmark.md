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

## SQLite Performance (Lua)

Dedicated SQLite benchmark (`bench_db.sh`) measuring isolated workloads:

| Workload | req/s | rows/s | Avg Latency | Description |
|----------|-------|--------|-------------|-------------|
| Single INSERT | 29,096 | 29,096 | 4.35ms | One INSERT per request |
| Batch INSERT (10/txn) | 15,237 | 152,370 | 7.47ms | 10 INSERTs wrapped in `db.batch()` |
| SELECT 20 rows | 6,303 | — | 16.20ms | Indexed ORDER BY + LIMIT 20 |
| Mixed (INSERT + SELECT) | 5,635 | — | 17.84ms | 1 write + 1 read per request |

**Key tuning improvements** (vs untuned baseline):

| Change | Single write | Batch write (rows/s) |
|--------|-------------|---------------------|
| Before (WAL + defaults) | 18,120 req/s | — (no batch API) |
| After (full PRAGMA tuning + stmt cache + db.batch) | 29,096 req/s | 152,370 rows/s |
| **Improvement** | **+61%** | **8.4x** |

The biggest wins come from:
1. `synchronous=NORMAL` — eliminates per-commit fsync in WAL mode (+40-60% writes)
2. `db.batch()` transaction wrapping — amortizes commit overhead across N operations (8x+ for batch writes)
3. Prepared statement cache — eliminates repeated `sqlite3_prepare_v2()` for hot queries

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
sh bench.sh           # run both Lua and JS benchmarks (HTTP routing)
sh bench_db.sh        # run SQLite performance benchmark
RUNTIME=lua sh bench.sh   # Lua only
RUNTIME=js  sh bench.sh   # JS only
```

Tunable environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| THREADS | 4 | wrk thread count |
| CONNECTIONS | 100 | concurrent connections |
| DURATION | 10s | test duration |

## Performance Tuning

Hull applies SQLite performance PRAGMAs automatically at startup. These defaults are tuned for local-first desktop/server usage with good durability.

### Default PRAGMAs

| PRAGMA | Default | Effect |
|--------|---------|--------|
| `journal_mode` | WAL | Write-Ahead Logging — concurrent readers during writes |
| `synchronous` | NORMAL | Sync on WAL checkpoint only (not every commit). Safe in WAL mode; only risk is losing the last transaction on OS crash (not app crash). |
| `foreign_keys` | ON | Enforce referential integrity |
| `busy_timeout` | 5000 | Wait up to 5s on lock contention instead of failing immediately |
| `cache_size` | -16384 | 16 MB page cache (SQLite default is 2 MB) |
| `temp_store` | MEMORY | Temp tables and indexes in RAM instead of temp files |
| `mmap_size` | 268435456 | Memory-map up to 256 MB of the DB file for faster reads |
| `wal_autocheckpoint` | 1000 | Auto-checkpoint every ~4 MB of WAL growth |

### Statement Cache

A 32-entry LRU prepared statement cache eliminates repeated `sqlite3_prepare_v2()` calls for hot queries. Statements are reused across requests via `sqlite3_reset()` + `sqlite3_clear_bindings()`.

### Transaction Batching

Use `db.batch(fn)` to wrap multiple writes in a single transaction:

```lua
-- Lua: 10 inserts in one transaction
db.batch(function()
    for i = 1, 10 do
        db.exec("INSERT INTO events (kind, ts) VALUES (?, ?)", {"log", time.now()})
    end
end)
```

```js
// JS: 10 inserts in one transaction
import { db } from "hull:db";
db.batch(() => {
    for (let i = 0; i < 10; i++) {
        db.exec("INSERT INTO events (kind, ts) VALUES (?, ?)", ["log", time.now()]);
    }
});
```

Without `db.batch()`, each `db.exec()` is an implicit auto-commit transaction. Batching amortizes the per-transaction overhead and can improve write throughput by 8x or more.

### Shutdown Optimization

On graceful shutdown, Hull runs:
1. `PRAGMA optimize` — updates query planner statistics
2. `wal_checkpoint(TRUNCATE)` — merges WAL back into the main DB file
