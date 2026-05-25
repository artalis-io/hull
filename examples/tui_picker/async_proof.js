// async_proof.js — JS counterpart to async_proof.lua.
//
// Proves that tui.poll yields to the event loop in JS too: a
// background timer increments a counter every ~50ms while the main
// async function awaits tui.poll(-1). Pressing any key stops both
// and the counter is printed to stdout.
//
// Used by tests/e2e_tui.sh.

import { app } from "hull:app";
import { tui } from "hull:tui";

app.manifest({
    tui: true,
    modules: ["hull/tui@1"],
});

app.main(async (ctx) => {
    let counter = 0;
    let running = true;

    /* Background ticker via standard JS setTimeout-like primitives.
     * hull.sleep returns a Promise; recursive scheduling keeps
     * incrementing while main awaits tui.poll. */
    async function tick() {
        while (running) {
            await hull.sleep(50);
            counter += 1;
        }
    }
    tui.async(tick);

    tui.enter();
    let keyName = null;
    while (keyName === null) {
        tui.clear();
        tui.print(1, 1, "Press any key to stop the background ticker.");
        tui.print(1, 2, `Background counter: ${counter}`);
        tui.flush();
        const ev = await tui.poll(-1);
        if (ev && ev.kind === "key") keyName = ev.key;
    }
    tui.leave();

    running = false;
    await hull.sleep(60);

    ctx.stdout.write(`async_proof: counter=${counter} key=${keyName}\n`);
    return 0;
});
