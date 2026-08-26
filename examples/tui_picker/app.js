// TUI picker - Hull + JavaScript TUI example.
//
// Mirrors examples/tui_picker/app.lua via the hull:tui module's
// JavaScript helpers (tui.list backed by tui.run, same as Lua).
//
// Run:  hull run app.js
//
// Keys: ↑/↓ or j/k move, enter picks, q/Esc aborts.

import { app } from "hull:app";
import { tui } from "hull:tui";

app.manifest({
    tui: true,
    modules: ["hull/tui@1"],
});

const fruits = [
    "apple", "apricot", "banana", "blueberry", "cantaloupe", "cherry",
    "date", "elderberry", "fig", "grape", "honeydew", "kiwi",
    "lemon", "mango", "nectarine", "orange", "papaya", "pear",
    "raspberry", "strawberry", "tangerine", "watermelon",
];

app.main(async (ctx) => {
    const picked = tui.list(fruits, {
        title: "Pick a fruit (↑/↓, enter, q to quit):",
    });

    if (picked === null) {
        ctx.stderr.write("aborted\n");
        return 130;
    }

    ctx.stdout.write(`${fruits[picked]}\n`);
    return 0;
});
