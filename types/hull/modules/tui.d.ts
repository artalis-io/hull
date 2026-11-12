// hull:tui - terminal UI (app.main apps; manifest `tui: true`). See docs/tui_mode.md.
declare module "hull:tui" {
    /** A key press ("a", "enter", "up", "ctrl+c", "escape", ...). */
    export interface TuiKeyEvent {
        /** Event kind. */
        kind: "key";
        /** Key name. */
        key: string;
        /** The Unicode code point of a printable key. */
        codepoint?: number;
        /** Modifier bitmask (MOD_SHIFT | MOD_ALT | MOD_CTRL | MOD_SUPER). */
        mods: number;
        /** Shift held. */
        shift: boolean;
        /** Alt held. */
        alt: boolean;
        /** Ctrl held. */
        ctrl: boolean;
        /** Super held. */
        super: boolean;
    }
    /** The terminal was resized. */
    export interface TuiResizeEvent {
        /** Event kind. */
        kind: "resize";
        /** New column count. */
        cols: number;
        /** New row count. */
        rows: number;
    }
    /** A mouse event (after `enableMouse(true)`). */
    export interface TuiMouseEvent {
        /** Event kind. */
        kind: "mouse";
        /** Column (1-based). */
        x: number;
        /** Row (1-based). */
        y: number;
        /** SGR button code. */
        button: number;
    }
    /** Bracketed paste (after `enablePaste(true)`). */
    export interface TuiPasteEvent {
        /** Event kind. */
        kind: "paste";
        /** The pasted text. */
        text: string;
    }
    /** Focus change (after `enableFocus(true)`). */
    export interface TuiFocusEvent {
        /** Event kind. */
        kind: "focus";
        /** True on focus in. */
        in: boolean;
    }
    /** The terminal went away; every later poll returns this. */
    export interface TuiHangupEvent {
        /** Event kind. */
        kind: "hangup";
    }
    /** An event of no other kind. */
    export interface TuiNoneEvent {
        /** Event kind. */
        kind: "none";
    }
    /** Any terminal event. */
    export type TuiEvent = TuiKeyEvent | TuiResizeEvent | TuiMouseEvent | TuiPasteEvent | TuiFocusEvent | TuiHangupEvent | TuiNoneEvent;

    /** Text style; colors are 0xRRGGBB numbers (absent = terminal default). */
    export interface TuiStyle {
        /** Foreground color. */
        fg?: number | null;
        /** Background color. */
        bg?: number | null;
        /** Bold. */
        bold?: boolean;
        /** Dim. */
        dim?: boolean;
        /** Italic. */
        italic?: boolean;
        /** Underline. */
        underline?: boolean;
        /** Reverse video. */
        reverse?: boolean;
        /** Strikethrough. */
        strike?: boolean;
    }

    /** The terminal size. */
    export interface TuiSize {
        /** Columns. */
        cols: number;
        /** Rows. */
        rows: number;
    }

    /** What the terminal supports. */
    export interface TuiCaps {
        /** 24-bit color. */
        truecolor: boolean;
        /** 256 colors. */
        color256: boolean;
        /** Basic colors. */
        color: boolean;
        /** Mouse reporting. */
        mouse: boolean;
        /** Focus events. */
        focus: boolean;
        /** Kitty keyboard protocol. */
        kittyKbd: boolean;
        /** OSC 52 clipboard. */
        clipboard: boolean;
    }

    /** The background theme detected at acquire. */
    export type TuiTheme = "dark" | "light" | "unknown";

    /** The draw context `tui.run` hands to `draw` each frame. */
    export interface TuiDrawContext {
        /** Columns (updated on resize). */
        cols: number;
        /** Rows (updated on resize). */
        rows: number;
        /** Clear the frame. */
        clear(): void;
        /** Force a full redraw on the next flush. */
        invalidate(): void;
        /** Move the cursor (1-based). */
        move(x: number, y: number): void;
        /** Set the style for later prints. */
        style(opts: TuiStyle): void;
        /** Print text at (x, y), 1-based. */
        print(x: number, y: number, s: string): void;
        /** Write text at the cursor. */
        write(s: string): void;
        /** Emit changed cells. */
        flush(): void;
        /** The detected theme. */
        theme(): TuiTheme;
    }

    /** Options of `tui.run`. */
    export interface TuiRunOptions<T> {
        /** Draw one frame (called first, then after every event / tick). */
        draw(t: TuiDrawContext): void;
        /** Handle an event; return a non-null value to exit `run` with it. */
        onEvent?(ev: TuiEvent): T | null | undefined | void;
        /** -1 (default) blocks for input; N > 0 redraws after N ms without input; 0 polls. */
        tickMs?: number;
        /** Enable mouse events. */
        mouse?: boolean;
        /** Enable bracketed paste. */
        paste?: boolean;
        /** Enable focus events. */
        focus?: boolean;
        /** Enable the Kitty keyboard protocol. */
        kittyKbd?: boolean;
    }

    /** Options of `tui.list`. */
    export interface TuiListOptions {
        /** A title line. */
        title?: string;
        /** The initially selected index. */
        initial?: number;
        /** Visible rows. */
        height?: number;
        /** Redraw period (ms). */
        tickMs?: number;
    }

    /** Options of `tui.confirm`. */
    export interface TuiConfirmOptions {
        /** Redraw period (ms). */
        tickMs?: number;
    }

    /** Options of `tui.progress`. */
    export interface TuiProgressOptions {
        /** Bar width in cells (default 20). */
        width?: number;
    }

    /** Options of `tui.frame`. */
    export interface TuiFrameOptions {
        /** Left column (default 1). */
        x?: number;
        /** Top row (default 1). */
        y?: number;
        /** Width (default: to the right edge). */
        w?: number;
        /** Height (default: to the bottom). */
        h?: number;
        /** Border style (default "single"). */
        border?: "single" | "double" | "round" | "ascii";
        /** A title in the top edge. */
        title?: string;
        /** Style of the border. */
        style?: TuiStyle;
    }

    /** The inner area `tui.frame` passes to its callback (coordinates 1-based inside the border). */
    export interface TuiFrameInner {
        /** Inner left column. */
        x: number;
        /** Inner top row. */
        y: number;
        /** Inner width. */
        w: number;
        /** Inner height. */
        h: number;
        /** Print inside the frame, clipped to it. */
        print(ix: number, iy: number, s: unknown): void;
    }

    /** Options of `tui.input`. */
    export interface TuiInputOptions {
        /** Initial text. */
        initial?: string;
        /** Most code points (default 1024). */
        maxLen?: number;
        /** Mask the text. */
        password?: boolean;
        /** Redraw period (ms). */
        tickMs?: number;
    }

    /** The `hull:tui` module object. */
    export interface TuiModule {
        /** Take the terminal (raw mode, alternate screen). */
        enter(): void;
        /** Give the terminal back. */
        leave(): void;
        /** The terminal size. */
        size(): TuiSize;
        /** The terminal's capabilities. */
        caps(): TuiCaps;
        /** The detected background theme. */
        theme(): TuiTheme;
        /** Clear the frame. */
        clear(): void;
        /** Force a full redraw on the next flush. */
        invalidate(): void;
        /** Move the cursor (1-based). */
        move(x: number, y: number): void;
        /** Set the style for later prints (no argument resets). */
        style(opts?: TuiStyle): void;
        /** Print text at (x, y), 1-based. */
        print(x: number, y: number, s: string): void;
        /** Write text at the cursor. */
        write(s: string): void;
        /** Emit changed cells. */
        flush(): void;
        /** Next event; 0 polls once (sync); otherwise an event already queued returns at once, else a Promise. */
        poll(timeoutMs: 0): TuiEvent | null;
        /** Next event; 0 polls once (sync); otherwise an event already queued returns at once, else a Promise (-1 waits for ever). */
        poll(timeoutMs?: number | null): TuiEvent | null | Promise<TuiEvent | null>;
        /** Copy text to the system clipboard (OSC 52); throws when unsupported. */
        clipboardSet(text: string): void;
        /** Turn mouse reporting on or off. */
        enableMouse(on: boolean): void;
        /** Turn bracketed paste on or off. */
        enablePaste(on: boolean): void;
        /** Turn focus events on or off. */
        enableFocus(on: boolean): void;
        /** Turn the Kitty keyboard protocol on or off. */
        enableKittyKbd(on: boolean): void;
        /** The immediate-mode loop: draw, wait for an event, repeat until `onEvent` returns a value (or hangup: null). Synchronous. */
        run<T = unknown>(opts: TuiRunOptions<T>): T | null;
        /** A scrollable picker: the chosen index, or null when cancelled. */
        list(items: readonly unknown[], opts?: TuiListOptions): number | null;
        /** A y/N prompt. */
        confirm(msg: string, opts?: TuiConfirmOptions): boolean;
        /** A spinner frame for `state` and the next state. */
        spinner(state?: number): [frame: string, next: number];
        /** A progress bar string for 0..1, e.g. "[████░░] 67%". */
        progress(pct: number, opts?: TuiProgressOptions): string;
        /** Draw a bordered box and call `fn` with its inner area. */
        frame(opts: TuiFrameOptions, fn?: (inner: TuiFrameInner) => void): void;
        /** A one-line editor: the text, or null when cancelled. */
        input(prompt?: string, opts?: TuiInputOptions): string | null;
        /** Run `fn` detached (its errors are logged, never thrown). */
        async(fn: () => unknown): void;
        /** Shift modifier bit. */
        readonly MOD_SHIFT: number;
        /** Alt modifier bit. */
        readonly MOD_ALT: number;
        /** Ctrl modifier bit. */
        readonly MOD_CTRL: number;
        /** Super modifier bit. */
        readonly MOD_SUPER: number;
    }
    /** The terminal UI. */
    export const tui: TuiModule;
}
