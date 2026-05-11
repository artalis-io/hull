<!-- minimal -->
## Deployment

Hull apps compile to single binaries with no runtime dependencies.

```bash
hull build myapp/          # compile to single binary
./myapp/build/app           # run (listens on port 3000)
./myapp/build/app -p 8080   # custom port
```

**Config generator:** `hull deploy` generates deployment configs from the app's manifest.

```bash
hull deploy dockerfile myapp/          # Dockerfile + .dockerignore
hull deploy systemd myapp/ --name app  # systemd service + install script
hull deploy fly myapp/ --region lax    # fly.toml (+ Dockerfile if missing)
hull agent deploy myapp/               # JSON deployment readiness analysis
```

**Sandbox:** Kernel-level enforcement (pledge/unveil on Linux, Seatbelt on macOS) restricts the binary to declared capabilities only.

**Manifest:** Declares what the app can access.
```lua
app.manifest({
    fs = { read = {"data/"}, write = {"uploads/"} },
    env = { "API_KEY", "DATABASE_URL" },
    hosts = { "api.stripe.com" },
})
```

<!-- compact -->
## Deploy Targets

| Target | Command | Output |
|--------|---------|--------|
| Docker | `hull deploy dockerfile myapp/` | `Dockerfile` + `.dockerignore` |
| systemd | `hull deploy systemd myapp/` | `deploy/<name>.service` + `deploy/install.sh` |
| Fly.io | `hull deploy fly myapp/` | `fly.toml` + `Dockerfile` |

**Common flags:** `--port N`, `--name NAME`, `-o DIR`, `--force` (overwrite).

**Dockerfile flags:** `--scratch` (default), `--distroless`, `--sign` (add verify step).

**systemd flags:** `--user NAME` (default: `hull`), `--install-dir PATH`, `--data-dir PATH`.

**Fly flags:** `--region CODE` (default: `iad`), `--memory N` (default: 256).

Generated configs adapt to the app automatically:
- CA bundle only when manifest declares `hosts`
- ENV declarations from manifest `env` array
- VOLUME/mounts only for database apps
- systemd hardening with 17 security directives

## Runtime Flags

| Flag | Effect |
|------|--------|
| `-p PORT` | Listen port (default: 3000) |
| `--audit` | Log all capability calls as JSON to stderr |
| `--no-sandbox` | Disable kernel sandbox (debugging only) |
| `--no-migrate` | Skip automatic migration on startup |
| `--max-instructions N` | Per-request instruction limit (default: 100M) |
| `--agent` | Enable sidecar files for AI agent integration |

Environment variable overrides: `HULL_AUDIT=1`, `HULL_MAX_INSTRUCTIONS=N`, `PORT=N`.

## Manifest Capabilities

- **`fs`** — filesystem paths the app can read/write (relative to app dir)
- **`env`** — environment variables the app can read (max 32)
- **`hosts`** — HTTP hosts the app can make outbound requests to

Anything not declared is blocked. Violations cause errors (macOS: EPERM) or process termination (Linux: SIGKILL).

## Sandbox Phases

1. **Phase 1 (load):** Restricts syscalls during module loading. Blocks `exec`, `fork`, `proc`.
2. **Phase 2 (run):** After manifest extraction, applies full restrictions. Filesystem unveil seals paths. Network limited to declared hosts.

## Database

SQLite database is created at `app_dir/db.sqlite` by default. Migrations run automatically on startup unless `--no-migrate` is passed.

<!-- full -->
## Production Deployment

### Manual

```bash
# Build and sign
hull keygen
hull build myapp/ --sign
hull verify myapp/build/app

# Deploy
scp myapp/build/app server:/opt/myapp/app
ssh server 'chmod +x /opt/myapp/app && /opt/myapp/app -p 8080'
```

### Docker

```bash
hull deploy dockerfile myapp/ --sign
# Copy hull binary into myapp/
docker build -t myapp myapp/
docker run -p 8080:8080 -v data:/data myapp
```

The generated Dockerfile uses a multi-stage build: `debian:bookworm-slim` build stage compiles the app with `hull build`, then copies the single binary to a `FROM scratch` runtime stage. No shell, no package manager, no libc in the final image.

### systemd

```bash
hull deploy systemd myapp/ --name myapp --user webapp
hull build myapp/ --output app
# Review deploy/install.sh, then run the commands it prints
```

The generated service unit includes defense-in-depth hardening: `NoNewPrivileges`, `ProtectSystem=strict`, `PrivateTmp`, `SystemCallFilter=@system-service`, `MemoryDenyWriteExecute`, and more — layered on top of hull's own kernel sandbox.

### Fly.io

```bash
hull deploy fly myapp/ --region lax --memory 512
# Copy hull binary into myapp/
fly deploy
```

### Agent Introspection

```bash
hull agent deploy myapp/
```

Returns JSON with deployment readiness analysis: runtime, manifest contents, file counts (migrations, templates, static, compute, shaders), existing configs, and recommendations.

## Audit Logging

`--audit` or `HULL_AUDIT=1` logs every capability call as JSON to stderr:

```json
{"cap":"db.query","sql":"SELECT * FROM users WHERE id = ?","params":[1]}
{"cap":"fs.read","path":"data/config.json"}
{"cap":"http.request","method":"GET","url":"https://api.stripe.com/v1/charges"}
```

Zero overhead when disabled (single branch check).

## Cross-Platform Binary

Using Cosmopolitan, build a single binary for all platforms:

```bash
make platform-cosmo
make CC=cosmocc EMBED_PLATFORM=cosmo
hull build myapp/
# Result: single APE binary runs on Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD
```

## Security Checklist

- Always declare the minimal manifest (least privilege)
- Sign binaries with `hull build --sign` and verify before deployment
- Never deploy with `--no-sandbox`
- Use `--audit` in staging to verify capability usage matches expectations
- Set `--max-instructions` to prevent runaway requests
- Run `hull verify` on the binary before deploying to confirm integrity
- Use `hull deploy` to generate hardened deployment configs (systemd unit includes 17 security directives)
