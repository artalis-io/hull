-- --vm-thread turned on by the manifest alone (tests/e2e_vm_thread.sh).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

app.manifest({
    modules = { "hull/http-server@1" },
    limits = { vm_thread = true, vm_thread_grace_ms = 400,
               vm_thread_ceiling_ms = 30000 },
})

app.get("/ping", function(_req, res) res:json({ ok = true }) end)
app.get("/stuck", function(req, res)
    hull._test_native_sleep(tonumber(req.query.ms) or 3000)
    res:json({ done = true })
end)
