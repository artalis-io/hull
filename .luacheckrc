globals = { "app", "db", "time", "json", "crypto", "log", "env", "fs", "tool", "test", "http", "smtp", "hull", "worker", "server", "compute", "gpu", "image", "ws", "__hull_exe", "_template" }
std = "lua54"
exclude_files = { "stdlib/lua/vendor/", "vendor/" }
max_line_length = false
unused_args = false
ignore = { "21./_.*" }

-- The C unit-test harness (tests/hull/runtime/lua/test_lua.c) gives the
-- stdlib test scripts __test_hooks(name): a module's test hooks, which it
-- no longer exports (audit 16).
files["stdlib/lua/hull/tests/"] = { read_globals = { "__test_hooks" } }
