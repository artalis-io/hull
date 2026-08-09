-- hull.feature_specs - the single source of truth per composable feature.
--
-- One declarative record per `--with=<name>` feature. `hull build` (compose
-- step) and any future cross-file consumer derive from this table, so adding a
-- feature is one row here + a `make feature-<name>` archive + a release-pipeline
-- entry. Extracted from build.lua's main() (Phase 3 of the build modularization,
-- docs/build_modularization.md) so the registry is reusable and not buried in a
-- 1.5k-line function. Pure data: every value is a literal, no runtime state.
--
-- Fields:
--   backend / type / hook  A feature that contributes a backend to a base hook
--                          returning an ARRAY (several features may feed one
--                          hook) carries these: the compose step generates one
--                          strong collector filling `hook` with `backend` (of C
--                          type `type`). A feature whose base hooks each have
--                          exactly one provider carries none of these; instead:
--   whole_archive = true   Force-load the archive so the strong overrides
--                          (spread across object files, no single backend symbol
--                          to anchor on) all get pulled. A plain per-feature LINK
--                          attribute, sibling to `cxx` - not a second feature kind.
--   cxx = true             Needs a system compiler + a C++ runtime.
--   base_group = true      The archive references base symbols the composing app
--                          doesn't otherwise pull, so GNU ld needs the platform
--                          lib + archive wrapped in --start-group at compose.
--   libs                   Extra link libs that cannot live in the .a, split
--                          { darwin = {...}, other = {...} }.

return {
    -- sqlite: the vendored SQLite engine + backend + UDF + agent
    -- introspection in libhull_feature-sqlite.a (`make feature-sqlite`),
    -- filling the same hl_db_feature_backends hook. Unlike the others,
    -- SQLite is Hull's DEFAULT backend composed onto a SQLite-less DB-core
    -- base (built HL_SQLITE_FEATURE=1); docs/sqlite_feature.md, Phase B.
    -- `base_group = true` because the archive references base symbols
    -- (db_registry, the _hull_* guard, agent write_error, vfs, migrate) and
    -- the base's generated override references the archive's backend, so the
    -- compose wraps platform lib + archive in --start-group for GNU ld.
    sqlite = {
        backend    = "hl_db_backend_sqlite",
        type       = "HlDbBackend",
        hook       = "hl_db_feature_backends",
        cxx        = false,
        base_group = true,
        libs       = { darwin = {}, other = {} },
    },
    duckdb = {
        backend = "hl_db_backend_duckdb",
        type    = "HlDbBackend",
        hook    = "hl_db_feature_backends",
        cxx     = true,
        libs    = { darwin = { "-lc++" }, other = { "-lstdc++", "-ldl" } },
    },
    -- postgres: pure-C PostgreSQL wire backend in libhull_feature-postgres.a
    -- (`make feature-postgres`), filling the same hl_db_feature_backends hook
    -- as duckdb. No vendored engine / no extra link libs. `base_group = true`
    -- because the backend references base crypto (SCRAM) + tls_client
    -- (sslmode) that a DB-only app doesn't otherwise pull, so the compose must
    -- wrap the platform lib + this archive in --start-group for GNU ld.
    postgres = {
        backend    = "hl_db_backend_postgres",
        type       = "HlDbBackend",
        hook       = "hl_db_feature_backends",
        cxx        = false,
        base_group = true,
        libs       = { darwin = {}, other = {} },
    },
    -- mysql: pure-C MySQL/MariaDB wire backend in libhull_feature-mysql.a
    -- (`make feature-mysql`), one backend serving both mysql:// and
    -- mariadb://. Identical shape to postgres — fills hl_db_feature_backends,
    -- references base crypto (native/caching_sha2 auth) + tls_client
    -- (sslmode), so base_group = true (--start-group at compose on GNU ld).
    mysql = {
        backend    = "hl_db_backend_mysql",
        type       = "HlDbBackend",
        hook       = "hl_db_feature_backends",
        cxx        = false,
        base_group = true,
        libs       = { darwin = {}, other = {} },
    },
    -- valkey: pure-C Valkey/Redis KV backend in libhull_feature-valkey.a
    -- (`make feature-valkey`). The FIRST non-SQL connection feature: it fills
    -- its OWN hook hl_kv_feature_backends (NOT hl_db_feature_backends), with C
    -- type HlKvBackend, so the codegen emits a separate collector and it composes
    -- side by side with the DB features. Pure C, no vendored engine. References
    -- base crypto (auth) + tls_client (rediss sslmode), so base_group = true
    -- (--start-group at compose on GNU ld). Selection is explicit --with=valkey
    -- (redis:// DSNs are often $VAR env-refs, invisible at build time), never
    -- auto-inferred.
    valkey = {
        backend    = "hl_kv_backend_valkey",
        type       = "HlKvBackend",
        hook       = "hl_kv_feature_backends",
        cxx        = false,
        base_group = true,
        libs       = { darwin = {}, other = {} },
    },
    -- gpu: wgpu-native backend, isolated in libhull_feature-gpu.a
    -- (`make feature-gpu`). Base ships the generic gpu dispatch layer +
    -- the weak hl_gpu_feature_backends hook; this fills it. C (no cxx). The
    -- frameworks / -lvulkan can't live in the .a so they're emitted here.
    gpu = {
        backend = "hl_gpu_backend_wgpu", type = "HlGpuBackend",
        hook = "hl_gpu_feature_backends", cxx = false,
        libs = { darwin = { "-framework", "Metal", "-framework", "QuartzCore",
                            "-framework", "CoreGraphics", "-framework", "Foundation" },
                 other  = { "-lvulkan" } },
    },
    -- tui: the whole TUI subsystem (cap + both runtime bridges) in
    -- libhull_feature-tui.a (`make feature-tui`). Its base hooks
    -- (hl_tui_feature_present / register_lua / register_js) each have one
    -- provider, and the strong overrides live across several object files
    -- with no single backend symbol, so the archive is whole-archive-linked
    -- (force_load) rather than pull-by-backend-symbol. Pure C, no extra libs
    -- (POSIX termios).
    tui = {
        whole_archive = true, cxx = false,
        libs = { darwin = {}, other = {} },
    },
}
