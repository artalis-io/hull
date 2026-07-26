-- Minimal app.main fixture for the self-build smoke.
--
-- The produced binary is a slim app-runner (hl_app_run), NOT the hull CLI, so
-- the old "hull2 builds hull3" chain no longer applies. This app just exits 0,
-- so `make self-build` can build it and RUN it to prove `hull build` yields a
-- runnable single-runtime binary end to end.
app.manifest({ name = "selfbuild", version = "0.0.1", modules = {} })
app.main(function(_ctx)
    return 0
end)
