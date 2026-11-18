# Build plugin demonstration (tests/e2e_plugins.sh)

`app_lua` and `app_js` are the same app in Lua and JavaScript: the same two
`@demo` declarations and one `@other`. `inventory_lua` and `inventory_js` are
the same build plugin in Lua and JavaScript: it lists the `@demo`
declarations it is shown, by name and annotation value, as one artifact.

The e2e copies a plugin into an app as `plugins/inventory`, names it in
`hull.build.json` and runs `hull plugins run`: all four pairings must commit
byte-identical outputs and diagnostics.
