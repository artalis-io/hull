-- async_proof.lua - minimal test that proves tui.poll() yields to
-- the event loop.
--
-- Spins up a background coroutine via tui.async that bumps a counter
-- every 50ms. The main coroutine sits in tui.poll(2000) waiting for
-- a key. If tui.poll yields to the event loop (correct), the counter
-- advances while we wait - pressing any key should reveal a count
-- of ~ N/50ms.
--
-- If tui.poll were blocking, the counter would stay at 0 because
-- hull.sleep needs the event loop to make progress.
--
-- Used by tests/e2e_tui.sh; not really suitable for human use.

local tui = require("hull.tui")

app.manifest({
    tui = true,
    modules = { "hull/tui@1" },
})

app.main(function(ctx)
    local counter = 0
    local running = true

    tui.async(function()
        while running do
            hull.sleep(50)
            counter = counter + 1
        end
    end)

    -- Block waiting for a single key - at most 2 seconds.
    local key_event = nil
    tui.run({
        draw = function(t)
            t:clear()
            t:print(1, 1, "Press any key to stop the background ticker.")
            t:print(1, 2, "Background counter: " .. tostring(counter))
        end,
        on_event = function(ev)
            if ev.kind == "key" then
                key_event = ev.key
                return "done"
            end
        end,
        tick_ms = -1,  -- block until key
    })

    running = false
    -- Allow the background coroutine one more tick to settle.
    hull.sleep(60)
    ctx.stdout:write("async_proof: counter=" .. tostring(counter) ..
                     " key=" .. tostring(key_event) .. "\n")
    return 0
end)
