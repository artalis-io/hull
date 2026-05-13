# Known Limitations

All limits are defined in `include/hull/limits.h` and can be overridden at compile time via `-D`.

## Module System

| Limit | Value | Define | Rationale |
|-------|-------|--------|-----------|
| Max module path length | 4096 bytes | `HL_MODULE_PATH_MAX` | Matches `PATH_MAX` on most POSIX systems |
| Max module file size | 10 MB | `HL_MODULE_MAX_SIZE` | Prevents OOM from pathological files; 10 MB is far above any reasonable module |

## HTTP / Body

| Limit | Value | Define | Rationale |
|-------|-------|--------|-----------|
| Request body max size | 1 MB | `HL_BODY_MAX_SIZE` | Default buffer reader limit; large uploads should use streaming |
| Query string buffer | 4096 bytes | `HL_QUERY_BUF_SIZE` | Query strings exceeding this are truncated during parsing |
| Route param name | 256 bytes | `HL_PARAM_NAME_MAX` | Param names (`:id`, `:slug`) are copied into a fixed buffer |

## Server Defaults

| Limit | Value | Define | Rationale |
|-------|-------|--------|-----------|
| Max routes | 256 | `HL_MAX_ROUTES` | Static route allocation array; increase if app has more routes |
| Default port | 3000 | `HL_DEFAULT_PORT` | Convention for development servers |
| Default max connections | 256 | `HL_DEFAULT_MAX_CONN` | Keel connection pool size |
| Default read timeout | 30 s | `HL_DEFAULT_READ_TIMEOUT_MS` | Idle connection timeout |

## Crypto

| Limit | Value | Define | Rationale |
|-------|-------|--------|-----------|
| crypto.random() max bytes | 65536 | `HL_RANDOM_MAX_BYTES` | Prevents accidental multi-MB allocations |
| PBKDF2 iterations | 100000 | `HL_PBKDF2_ITERATIONS` | OWASP minimum recommendation for PBKDF2-HMAC-SHA256 |

## Runtime Memory

| Limit | Value | Define | Rationale |
|-------|-------|--------|-----------|
| Lua heap limit | 64 MB | `HL_LUA_DEFAULT_HEAP` | Custom allocator refuses allocations above this |
| JS heap limit | 64 MB | `HL_JS_DEFAULT_HEAP` | QuickJS `JS_SetMemoryLimit` |
| JS stack limit | 1 MB | `HL_JS_DEFAULT_STACK` | QuickJS `JS_SetMaxStackSize` |
| JS GC threshold | 256 KB | `HL_JS_GC_THRESHOLD` | Bytes allocated before cycle GC triggers |

## Sandbox (pledge) + outbound HTTPS on modern Linux

On Linux glibc ≥ 2.32 (Ubuntu 22.04+, Debian 12+, RHEL 9+), `getaddrinfo()` resolves hostnames by:

1. Opening an `AF_UNIX` socket to talk to `systemd-resolved` or `nscd` via nsswitch IPC
2. Opening an `AF_NETLINK` socket to enumerate network interfaces
3. Using `sendmmsg(2)` to send batched A + AAAA DNS queries in one call

The jart/pledge polyfill (`vendor/pledge`) bundles pledge promises that map to seccomp-bpf rules. The `dns`, `netlink`, and `unix` promises cover (2) and (1) above, but the polyfill's `kPledgeDns[]` does not include `__NR_sendmmsg` — only `__NR_sendto`. So `http.fetch("https://...")` to an external host under the default sandbox can fail with `pledge sendfd for sendmmsg` on these Linux versions.

Workarounds:
- `--no-sandbox` (dev-mode disable kernel enforcement)
- Pre-resolve the hostname to an IP and pass that as a literal address
- Run `hull` behind a local proxy/sidecar that does DNS for it
- Build hull as a Cosmopolitan APE (cosmo's libc avoids these glibc-specific paths)

Fixing properly requires either extending the polyfill's `dns` syscall list or contributing the fix upstream to jart/pledge. Tracked separately; the `e2e_ca_bundle.sh` test uses `--no-sandbox` so it isolates CA-bundle correctness from this orthogonal limitation. Sandbox + local-only HTTP (`http.fetch("http://127.0.0.1:...")` ) is unaffected and works as expected.
