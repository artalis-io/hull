// hull:compute - WASM compute plugins (compute/<name>.wasm). See docs/app_api_reference.md.
declare namespace Hull {
    interface WasmBuffer {
        /** Byte length (0 once closed). */
        readonly length: number;
        /** A copy of the bytes, or null once closed. */
        bytes(): ArrayBuffer | null;
        /** Release the bytes (or the pooled instance a zero-copy result holds). Idempotent. */
        close(): void;
    }
}

declare module "hull:compute" {
    /** A call's input: a string (its UTF-8 bytes), an ArrayBuffer, a typed array, or an open WasmBuffer / MappedBuffer. */
    export type ComputeInput = string | ArrayBuffer | ArrayBufferView | Hull.WasmBuffer | Hull.MappedBuffer;

    /** Per-call limits; each is clamped to the manifest / CLI ceiling. */
    export interface ComputeLimits {
        /** Instruction gas (interpreter only). */
        gas?: number;
        /** Wall-clock bound (ms); default 10 s. */
        timeoutMs?: number;
    }

    /** Limits that size an instance. */
    export interface ComputeMemoryOptions {
        /** Guest heap bytes. */
        heap?: number;
        /** Guest stack bytes. */
        stack?: number;
    }

    /** Input / output size limits. */
    export interface ComputeIoOptions {
        /** Largest input accepted (bytes). */
        maxInput?: number;
        /** Largest output accepted (bytes). */
        maxOutput?: number;
    }

    /** A read-only file mapping a call exposes to the guest by name. */
    export interface ComputeSpan {
        /** Span name (1..63 bytes, unique within the call). */
        name: string;
        /** An open MappedBuffer (`fs.mmap`). */
        buffer: Hull.MappedBuffer;
    }

    /** Options of a call on a persistent instance. */
    export interface ComputeInstanceCallOptions extends ComputeLimits, ComputeIoOptions {
        /** Return the output as a zero-copy WasmBuffer instead of an ArrayBuffer. */
        buffer?: boolean;
        /** Mapped spans for this call (at most 8). */
        spans?: ComputeSpan[];
    }

    /** Options of `compute.call` / `compute.async.call`. */
    export interface ComputeCallOptions extends ComputeInstanceCallOptions, ComputeMemoryOptions {}

    /** Options of `compute.instance`. */
    export interface ComputeInstanceOptions extends ComputeLimits, ComputeIoOptions, ComputeMemoryOptions {}

    /** Options of `compute.stream`. */
    export interface ComputeStreamOptions extends ComputeLimits, ComputeMemoryOptions {
        /** Bytes per chunk. */
        chunkSize?: number;
    }

    /** A file under the app's fs grants, as a stream input or output. */
    export interface ComputeFileRef {
        /** Path relative to the app (checked against manifest `fs`). */
        file: string;
    }

    /** Receives each output chunk of `compute.stream`. */
    export type ComputeStreamCallback = (chunk: ArrayBuffer, index: number, isLast: boolean) => void;

    /** Non-blocking calls on an instance. */
    export interface WasmInstanceAsync {
        /** Run the instance on the thread pool; resolves to the output. */
        call(input: ComputeInput, opts: ComputeInstanceCallOptions & { buffer: true }): Promise<Hull.WasmBuffer>;
        /** Run the instance on the thread pool; resolves to the output. */
        call(input: ComputeInput, opts?: ComputeInstanceCallOptions & { buffer?: false }): Promise<ArrayBuffer>;
        /** Run the instance on the thread pool; resolves to the output. */
        call(input: ComputeInput, opts: ComputeInstanceCallOptions): Promise<ArrayBuffer | Hull.WasmBuffer>;
    }

    /** A persistent WASM instance (`compute.instance`); its memory survives between calls. */
    export interface WasmInstance {
        /** Call the module's entry point synchronously. */
        call(input: ComputeInput, opts: ComputeInstanceCallOptions & { buffer: true }): Hull.WasmBuffer;
        /** Call the module's entry point synchronously. */
        call(input: ComputeInput, opts?: ComputeInstanceCallOptions & { buffer?: false }): ArrayBuffer;
        /** Call the module's entry point synchronously. */
        call(input: ComputeInput, opts: ComputeInstanceCallOptions): ArrayBuffer | Hull.WasmBuffer;
        /** Destroy the instance. Idempotent. */
        close(): void;
        /** Non-blocking calls. */
        readonly async: WasmInstanceAsync;
        /** The module name, or null once closed. */
        readonly name: string | null;
        /** Whether the instance is closed. */
        readonly closed: boolean;
    }

    /** Non-blocking compute. */
    export interface ComputeAsync {
        /** Run module `name` on the thread pool; resolves to the output. */
        call(name: string, input: ComputeInput, opts: ComputeCallOptions & { buffer: true }): Promise<Hull.WasmBuffer>;
        /** Run module `name` on the thread pool; resolves to the output. */
        call(name: string, input: ComputeInput, opts?: ComputeCallOptions & { buffer?: false }): Promise<ArrayBuffer>;
        /** Run module `name` on the thread pool; resolves to the output. */
        call(name: string, input: ComputeInput, opts: ComputeCallOptions): Promise<ArrayBuffer | Hull.WasmBuffer>;
    }

    /** The `hull:compute` module object. Failures throw (InternalError "compute.call: <reason>"). */
    export interface ComputeModule {
        /** Whether the WASM runtime is initialised. */
        available(): boolean;
        /** Run module `name` synchronously (blocks the event loop). */
        call(name: string, input: ComputeInput, opts: ComputeCallOptions & { buffer: true }): Hull.WasmBuffer;
        /** Run module `name` synchronously (blocks the event loop). */
        call(name: string, input: ComputeInput, opts?: ComputeCallOptions & { buffer?: false }): ArrayBuffer;
        /** Run module `name` synchronously (blocks the event loop). */
        call(name: string, input: ComputeInput, opts: ComputeCallOptions): ArrayBuffer | Hull.WasmBuffer;
        /** Load (and cache) module `name` ahead of its first call -> true. */
        load(name: string): true;
        /** Copy bytes into a Hull-owned WasmBuffer. */
        buffer(input: string | ArrayBuffer | ArrayBufferView): Hull.WasmBuffer;
        /** Create a persistent instance of module `name`. */
        instance(name: string, opts?: ComputeInstanceOptions): WasmInstance;
        /** Load the module's data segments from compute/ (segment null: all of them) -> true. */
        segment(module: string, segment: string | null): true;
        /** Load or replace a named data segment from bytes -> true (a page-aligned MappedBuffer is used in place). */
        segment(module: string, segment: string, data: string | ArrayBuffer | ArrayBufferView | Hull.MappedBuffer | null): true;
        /** Stream input through module `name` chunk by chunk; output goes to a callback -> true. */
        stream(name: string, input: ComputeInput | ComputeFileRef, output: ComputeStreamCallback, opts?: ComputeStreamOptions): true;
        /** Stream input through module `name` chunk by chunk; output goes to a file -> true. */
        stream(name: string, input: ComputeInput | ComputeFileRef, output: ComputeFileRef, opts?: ComputeStreamOptions): true;
        /** Stream input through module `name` chunk by chunk; returns the concatenated output. */
        stream(name: string, input: ComputeInput | ComputeFileRef, opts?: ComputeStreamOptions | null): ArrayBuffer;
        /** Stream input through module `name` chunk by chunk; returns the concatenated output. */
        stream(name: string, input: ComputeInput | ComputeFileRef, output: null | undefined, opts?: ComputeStreamOptions): ArrayBuffer;
        /** Non-blocking compute. */
        readonly async: ComputeAsync;
    }
    /** WASM compute. */
    export const compute: ComputeModule;
}
