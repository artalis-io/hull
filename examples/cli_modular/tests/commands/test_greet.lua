-- Tests use test.run_main to synthesise argv/stdin/env and capture
-- the exit code + stdout/stderr. As of v0.1.0 the Phase 2 CLI test
-- harness is still being shaped - for now invoke through `hull run`
-- in the project root.

test("greet: smoke", function()
    test.eq(1, 1)  -- placeholder; replace with test.run_main once shipped
end)
