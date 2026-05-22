/**
 * @file hull:tui
 * @module hull:tui
 * @description Terminal UI module — public JS surface.
 *
 * Wraps the native bindings (`hull:tui` registered from C) with the
 * canonical `tui.run` immediate-mode loop and helper widgets
 * (`tui.list`, `tui.confirm`, `tui.spinner`, `tui.progress`,
 * `tui.frame`, `tui.input`). Mirrors stdlib/lua/hull/tui.lua.
 *
 * @license AGPL-3.0-or-later
 */

import { tui as native } from "hull:_tui";

function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Per-frame draw context handed to the user's `draw` callback. */
function makeDrawCtx() {
    const { cols, rows } = native.size();
    return {
        cols, rows,
        clear:      () => native.clear(),
        invalidate: () => native.invalidate(),
        move:       (x, y) => native.move(x, y),
        style:      (opts) => native.style(opts),
        print:      (x, y, s) => native.print(x, y, s),
        write:      (s) => native.write(s),
        flush:      () => native.flush(),
        theme:      () => native.theme(),
    };
}

/* ── tui.run ─────────────────────────────────────────────────── */

function run(opts) {
    if (!opts || typeof opts.draw !== "function") {
        throw new TypeError("tui.run: opts.draw must be a function");
    }
    const { draw, onEvent } = opts;
    const tickMs = (typeof opts.tickMs === "number") ? opts.tickMs : -1;

    native.enter();
    if (opts.mouse)     native.enableMouse(true);
    if (opts.paste)     native.enablePaste(true);
    if (opts.focus)     native.enableFocus(true);
    if (opts.kittyKbd)  native.enableKittyKbd(true);

    let ctx = makeDrawCtx();
    let exitToken = null;

    const refresh = () => {
        const { cols, rows } = native.size();
        ctx.cols = cols;
        ctx.rows = rows;
    };

    const safeDraw = () => {
        try {
            draw(ctx);
            native.flush();
        } catch (e) {
            native.leave();
            throw e;
        }
    };

    safeDraw();

    while (exitToken === null) {
        const ev = native.poll(tickMs);
        if (ev) {
            if (ev.kind === "resize") refresh();
            if (onEvent) {
                const r = onEvent(ev);
                if (r !== undefined && r !== null) exitToken = r;
            }
        }
        if (exitToken === null) safeDraw();
    }

    native.leave();
    return exitToken;
}

/* ── tui.list ────────────────────────────────────────────────── */

function list(items, opts = {}) {
    const count = items.length;
    if (count === 0) return null;

    const title = opts.title;
    let cursor = clamp(opts.initial ?? 0, 0, count - 1);
    let top = 0;

    const result = run({
        draw(t) {
            t.clear();
            let y = 1;
            if (title) {
                t.style({ bold: true });
                t.print(1, y, title);
                t.style({});
                y += 1;
            }
            const h = clamp(opts.height ?? (t.rows - (title ? 1 : 0)), 1, count);
            if (cursor < top) top = cursor;
            if (cursor > top + h - 1) top = cursor - h + 1;
            for (let i = 0; i < h; i++) {
                const idx = top + i;
                if (idx >= count) break;
                if (idx === cursor) {
                    t.style({ reverse: true });
                    t.print(1, y + i, "> " + String(items[idx]));
                    t.style({});
                } else {
                    t.print(1, y + i, "  " + String(items[idx]));
                }
            }
        },
        onEvent(ev) {
            if (ev.kind !== "key") return;
            const k = ev.key;
            if (k === "up" || k === "k")        cursor = clamp(cursor - 1, 0, count - 1);
            else if (k === "down" || k === "j") cursor = clamp(cursor + 1, 0, count - 1);
            else if (k === "pageup")            cursor = clamp(cursor - 10, 0, count - 1);
            else if (k === "pagedown")          cursor = clamp(cursor + 10, 0, count - 1);
            else if (k === "home")              cursor = 0;
            else if (k === "end")               cursor = count - 1;
            else if (k === "enter")             return cursor;
            else if (k === "escape" || k === "q" || k === "ctrl+c") return false;
        },
    });

    return (result === false) ? null : result;
}

/* ── tui.confirm ─────────────────────────────────────────────── */

function confirm(msg) {
    const result = run({
        draw(t) {
            t.clear();
            t.print(1, 1, String(msg) + " [y/N]");
        },
        onEvent(ev) {
            if (ev.kind !== "key") return;
            if (ev.key === "y" || ev.key === "Y") return true;
            if (ev.key === "n" || ev.key === "N") return false;
            if (ev.key === "enter") return false;
            if (ev.key === "escape" || ev.key === "ctrl+c") return false;
        },
    });
    return result === true;
}

/* ── tui.spinner ─────────────────────────────────────────────── */

const SPINNER_FRAMES = ["⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"];
function spinner(state = 0) {
    let next = state + 1;
    if (next >= SPINNER_FRAMES.length) next = 0;
    return [SPINNER_FRAMES[next], next];
}

/* ── tui.progress ────────────────────────────────────────────── */

function progress(pct, opts = {}) {
    const width = opts.width ?? 20;
    const p = clamp(pct ?? 0, 0, 1);
    const fill = Math.floor(p * width + 0.5);
    const bar = "█".repeat(fill) + "░".repeat(width - fill);
    const pctNum = Math.floor(p * 100 + 0.5);
    return `[${bar}] ${String(pctNum).padStart(3)}%`;
}

/* ── tui.frame ───────────────────────────────────────────────── */

const BORDERS = {
    single: { tl:"┌", tr:"┐", bl:"└", br:"┘", h:"─", v:"│" },
    double: { tl:"╔", tr:"╗", bl:"╚", br:"╝", h:"═", v:"║" },
    round:  { tl:"╭", tr:"╮", bl:"╰", br:"╯", h:"─", v:"│" },
    ascii:  { tl:"+", tr:"+", bl:"+", br:"+", h:"-", v:"|" },
};

function frame(opts, fn) {
    const { cols, rows } = native.size();
    const x = opts.x ?? 1;
    const y = opts.y ?? 1;
    const w = opts.w ?? (cols - x + 1);
    const h = opts.h ?? (rows - y + 1);
    if (w < 2 || h < 2) return;
    const b = BORDERS[opts.border ?? "single"] ?? BORDERS.single;

    if (opts.style) native.style(opts.style);

    let top = b.tl + b.h.repeat(w - 2) + b.tr;
    if (opts.title && w - 4 > 0) {
        let tt = String(opts.title);
        if (tt.length > w - 4) tt = tt.slice(0, w - 4);
        const prefix = b.tl + b.h + " ";
        const suffixLen = Math.max(0, w - prefix.length - tt.length - 2);
        top = prefix + tt + " " + b.h.repeat(suffixLen) + b.tr;
    }
    native.print(x, y, top);

    for (let i = 1; i < h - 1; i++) {
        native.print(x, y + i, b.v);
        native.print(x + w - 1, y + i, b.v);
    }
    native.print(x, y + h - 1, b.bl + b.h.repeat(w - 2) + b.br);

    if (opts.style) native.style({});

    if (fn) {
        fn({
            x: x + 1, y: y + 1, w: w - 2, h: h - 2,
            print(ix, iy, s) {
                if (ix < 1 || iy < 1 || ix > w - 2 || iy > h - 2) return;
                if (s.length > w - 2 - (ix - 1)) s = s.slice(0, w - 2 - (ix - 1));
                native.print(x + ix, y + iy, s);
            },
        });
    }
}

/* ── tui.input ───────────────────────────────────────────────── */

function input(prompt, opts = {}) {
    const initial = opts.initial ?? "";
    const maxLen  = opts.maxLen ?? 1024;
    const password = !!opts.password;
    const promptStr = prompt ?? "";

    let buf = initial;
    let cursor = buf.length;

    const result = run({
        draw(t) {
            t.clear();
            t.print(1, 1, promptStr);
            const display = password ? "*".repeat(buf.length) : buf;
            const x = promptStr.length + 1;
            t.print(x, 1, display);
            t.move(x + cursor, 1);
        },
        onEvent(ev) {
            if (ev.kind !== "key") return;
            const k = ev.key;
            if (k === "enter") return buf;
            if (k === "escape" || k === "ctrl+c") return false;
            if (k === "backspace") {
                if (cursor > 0) {
                    buf = buf.slice(0, cursor - 1) + buf.slice(cursor);
                    cursor -= 1;
                }
            } else if (k === "delete") {
                if (cursor < buf.length) buf = buf.slice(0, cursor) + buf.slice(cursor + 1);
            } else if (k === "left") {
                if (cursor > 0) cursor -= 1;
            } else if (k === "right") {
                if (cursor < buf.length) cursor += 1;
            } else if (k === "home") {
                cursor = 0;
            } else if (k === "end") {
                cursor = buf.length;
            } else if (k === "ctrl+u") {
                buf = buf.slice(cursor);
                cursor = 0;
            } else if (k === "ctrl+k") {
                buf = buf.slice(0, cursor);
            } else if (ev.codepoint && ev.codepoint >= 0x20 && !ev.ctrl && !ev.alt) {
                if (buf.length < maxLen) {
                    buf = buf.slice(0, cursor) + ev.key + buf.slice(cursor);
                    cursor += ev.key.length;
                }
            }
        },
    });

    return (result === false) ? null : result;
}

/* ── async helper ────────────────────────────────────────────── */

function asyncFn(fn) { return native.async(fn); }

/* ── exports ────────────────────────────────────────────────── */

const tui = {
    /* Re-export native primitives so consumers don't need two
     * imports. Mirrors stdlib/lua/hull/tui.lua which does the same. */
    enter: native.enter,
    leave: native.leave,
    size:  native.size,
    caps:  native.caps,
    theme: native.theme,

    clear:      native.clear,
    invalidate: native.invalidate,
    move:       native.move,
    style:      native.style,
    print:      native.print,
    write:      native.write,
    flush:      native.flush,
    poll:       native.poll,

    clipboardSet:  native.clipboardSet,
    enableMouse:   native.enableMouse,
    enablePaste:   native.enablePaste,
    enableFocus:   native.enableFocus,
    enableKittyKbd:native.enableKittyKbd,

    /* Public helpers (this file). */
    run, list, confirm, spinner, progress, frame, input,
    async: asyncFn,

    MOD_SHIFT: native.MOD_SHIFT,
    MOD_ALT:   native.MOD_ALT,
    MOD_CTRL:  native.MOD_CTRL,
    MOD_SUPER: native.MOD_SUPER,
};

export { tui };
