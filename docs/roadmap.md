# Hull — Roadmap & Benchmarks

## Benchmark Baseline

Measured on GitHub Actions Ubuntu runner (2 threads, 50 connections, 5s duration via `wrk`).
Commit: `f1e3996` (2026-02-26).

### GET /health (no DB — pure runtime overhead)

| Runtime | Req/sec | Avg Latency | Max Latency |
|---------|--------:|------------:|------------:|
| Lua 5.4 | 98,531 | 500 us | 1.84 ms |
| QuickJS | 52,263 | 950 us | 1.73 ms |

### GET / (DB write + JSON response)

| Runtime | Req/sec | Avg Latency | Max Latency |
|---------|--------:|------------:|------------:|
| Lua 5.4 | 6,866 | 7.42 ms | 28.02 ms |
| QuickJS | 4,588 | 10.97 ms | 28.36 ms |

### GET /greet/:name (route param extraction)

| Runtime | Req/sec | Avg Latency | Max Latency |
|---------|--------:|------------:|------------:|
| Lua 5.4 | 102,204 | 485 us | 6.98 ms |
| QuickJS | 57,405 | 870 us | 7.71 ms |

### Summary

- Lua is ~1.9x faster than QuickJS across all benchmarks.
- Non-DB routes sustain 50k-100k req/s on a single CI VM core with sub-millisecond average latency.
- DB-bound routes (SQLite write per request) sustain 5k-7k req/s.
