// hull:web:htmx:toast - transient notifications via HX-Trigger.
declare module "hull:web:htmx:toast" {
    /** A toast level; unknown values collapse to "info". */
    export type ToastLevel = "info" | "success" | "warning" | "error";
    /** Options for `toast.show`. */
    export interface ToastOptions {
        /** Level (default "info"). */
        level?: ToastLevel;
        /** Auto-dismiss after this many ms (default 4000 client-side; 0 = never). */
        duration?: number;
        /** Stable id (client-side dedup). */
        id?: string | number;
    }
    /** Options for the level shorthands (the level is fixed). */
    export type ToastLevelOptions = Omit<ToastOptions, "level">;
    /** The `hull:web:htmx:toast` module object. */
    export interface ToastModule {
        /** Send a toast (sets HX-Trigger `{"toast": {...}}`). */
        show(res: Hull.Response, message: unknown, opts?: ToastOptions): void;
        /** Send an "info" toast. */
        info(res: Hull.Response, message: unknown, opts?: ToastLevelOptions): void;
        /** Send a "success" toast. */
        success(res: Hull.Response, message: unknown, opts?: ToastLevelOptions): void;
        /** Send a "warning" toast. */
        warning(res: Hull.Response, message: unknown, opts?: ToastLevelOptions): void;
        /** Send an "error" toast. */
        error(res: Hull.Response, message: unknown, opts?: ToastLevelOptions): void;
    }
    /** htmx toast widget. */
    export const toast: ToastModule;
    export default toast;
}
