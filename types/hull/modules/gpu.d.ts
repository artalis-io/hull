// hull:gpu - GPU compute via WGSL shaders (the `gpu` feature: hull build --with=gpu).
declare module "hull:gpu" {
    /** Bytes in: a string, an ArrayBuffer, a typed array, a WasmBuffer, a MappedBuffer or an Image's pixels. */
    export type GpuData = string | ArrayBuffer | ArrayBufferView | Hull.WasmBuffer | Hull.MappedBuffer | Hull.Image;
    /** A texture format. */
    export type GpuTextureFormat = "rgba8" | "r8" | "rgba16f" | "r32f";

    /** An adapter Hull can dispatch to. */
    export interface GpuDevice {
        /** Device index (the `device` option). */
        id: number;
        /** Adapter name. */
        name: string;
    }

    /** Selects the device (default 0). */
    export interface GpuDeviceOption {
        /** Device index from `gpu.devices()`. */
        device?: number;
    }

    /** Workgroup counts (each default 1). */
    export interface GpuWorkgroups {
        /** X count. */
        x?: number;
        /** Y count. */
        y?: number;
        /** Z count. */
        z?: number;
    }

    /** A storage buffer bound by name. */
    export interface GpuBufferDesc {
        /** The WGSL binding name, or a persistent buffer made with `gpu.buffer`. */
        name?: string;
        /** Initial contents. */
        data?: GpuData | null;
        /** Size in bytes (default: the data's length). */
        size?: number;
        /** Access (default per the shader's declaration). */
        usage?: "read" | "write" | "readwrite";
    }

    /** A texture bound by name. */
    export interface GpuTextureDesc {
        /** The WGSL binding name, or a persistent texture made with `gpu.texture`. */
        name?: string;
        /** A storage (writable) texture. */
        storage?: boolean;
        /** Width in texels. */
        width?: number;
        /** Height in texels. */
        height?: number;
        /** Texel format (default "rgba8"). */
        format?: GpuTextureFormat;
        /** Initial texels. */
        data?: GpuData | null;
    }

    /** Options of `gpu.dispatch`. */
    export interface GpuDispatchOptions extends GpuDeviceOption {
        /** Workgroup counts. */
        workgroups?: GpuWorkgroups;
        /** Uniform bytes (binding 0). */
        uniforms?: GpuData | null;
        /** Storage buffers (at most 16). */
        buffers?: GpuBufferDesc[];
        /** Textures (at most 8). */
        textures?: GpuTextureDesc[];
        /** Index of the buffer to read back (default 0, unless `outputTexture` is given); false reads nothing back. */
        output?: number | false;
        /** Index of the texture whose raw pixel bytes to read back; given without `output`, it replaces the buffer default. */
        outputTexture?: number;
        /** Bound (ms), clamped to 60 s. */
        timeout?: number;
    }

    /** Options of `gpu.async.dispatch` (no textures). */
    export interface GpuAsyncDispatchOptions extends GpuDeviceOption {
        /** Workgroup counts. */
        workgroups?: GpuWorkgroups;
        /** Uniform bytes (binding 0). */
        uniforms?: GpuData | null;
        /** Storage buffers (at most 16). */
        buffers?: GpuBufferDesc[];
        /** Index of the buffer to read back (default 0); false reads nothing back. */
        output?: number | false;
        /** Bound (ms), clamped to 60 s. */
        timeout?: number;
    }

    /** One stage of `gpu.pipeline`. */
    export interface GpuPipelineStage {
        /** A shader compiled with `gpu.compile` / `gpu.load`. */
        shader: string;
        /** Workgroup counts. */
        workgroups?: GpuWorkgroups;
        /** Uniform bytes. */
        uniforms?: GpuData | null;
        /** Storage buffers (at most 16 per stage). */
        buffers?: GpuBufferDesc[];
    }

    /** Options of `gpu.pipeline`. */
    export interface GpuPipelineOptions extends GpuDeviceOption {
        /** Buffers to read back, 0-indexed (default: stage 0, buffer 0 of the last stage). */
        outputs?: { stage: number; buffer: number }[];
        /** false reads nothing back. */
        output?: false;
        /** Bound (ms), clamped to 60 s. */
        timeout?: number;
    }

    /** Options of `gpu.buffer`. */
    export interface GpuBufferOptions extends GpuDeviceOption {
        /** Byte offset to write at (updating an existing buffer). */
        offset?: number;
    }

    /** Options of `gpu.bufferCopy`. */
    export interface GpuBufferCopyOptions extends GpuDeviceOption {
        /** Source byte offset. */
        srcOffset?: number;
        /** Destination byte offset. */
        dstOffset?: number;
        /** Bytes to copy (default: the rest of the source). */
        size?: number;
    }

    /** Options of `gpu.texture` (dimensions / format override an Image's). */
    export interface GpuTextureOptions extends GpuDeviceOption {
        /** Width in texels (required for raw bytes). */
        width?: number;
        /** Height in texels (required for raw bytes). */
        height?: number;
        /** Texel format (default "rgba8"). */
        format?: GpuTextureFormat;
        /** A storage (writable) texture. */
        storage?: boolean;
        /** Sampler filter (default nearest). */
        filter?: "nearest" | "linear";
        /** Sampler address mode (default clamp). */
        wrap?: "clamp" | "repeat" | "mirror";
    }

    /** Non-blocking GPU calls (run on the thread pool). */
    export interface GpuAsync {
        /** Compile WGSL source under `name` -> true. */
        compile(name: string, wgsl: string, opts?: GpuDeviceOption): Promise<true>;
        /** Compile `shaders/<name>.wgsl` -> true. */
        load(name: string, opts?: GpuDeviceOption): Promise<true>;
        /** Run a compiled shader; resolves to the read-back (empty when `output: false`). */
        dispatch(name: string, opts: GpuAsyncDispatchOptions): Promise<ArrayBuffer>;
        /** Run stages in order; resolves to one read-back, or an array for several outputs. */
        pipeline(stages: GpuPipelineStage[], opts?: GpuPipelineOptions): Promise<ArrayBuffer | ArrayBuffer[]>;
    }

    /** The `hull:gpu` module object. Failures throw ("gpu_busy", "compile_timeout", ...). */
    export interface GpuModule {
        /** Whether a GPU device is usable. */
        available(): boolean;
        /** The usable adapters. */
        devices(): GpuDevice[];
        /** Compile WGSL source under `name` -> true (a built binary compiles only its shaders/ unless manifest `gpu.dynamicShaders`). */
        compile(name: string, wgsl: string, opts?: GpuDeviceOption): true;
        /** Compile `shaders/<name>.wgsl` -> true. */
        load(name: string, opts?: GpuDeviceOption): true;
        /** Run a compiled shader with no read-back (`output: false`, no `outputTexture`) -> true. */
        dispatch(name: string, opts: GpuDispatchOptions & { output: false; outputTexture?: undefined }): true;
        /** Run a compiled shader; returns the read-back of buffer `output` (default 0), or of texture `outputTexture`. */
        dispatch(name: string, opts: GpuDispatchOptions & { output?: number }): ArrayBuffer;
        /** Run a compiled shader; returns the read-back, or true when nothing is read back (`output: false`, no `outputTexture`). */
        dispatch(name: string, opts: GpuDispatchOptions): ArrayBuffer | true;
        /** Run stages in order; one read-back, an array for several outputs, or true when nothing is read back. */
        pipeline(stages: GpuPipelineStage[], opts?: GpuPipelineOptions): ArrayBuffer | ArrayBuffer[] | true;
        /** Create or write a persistent buffer (null destroys it) -> true. */
        buffer(name: string, data: GpuData | null, opts?: GpuBufferOptions): true;
        /** Read a persistent buffer back. */
        bufferRead(name: string, opts?: GpuDeviceOption): ArrayBuffer;
        /** Copy between persistent buffers -> true. */
        bufferCopy(src: string, dst: string, opts?: GpuBufferCopyOptions): true;
        /** Create or write a persistent texture from an Image or raw bytes (null destroys it) -> true. */
        texture(name: string, data: GpuData | null, opts?: GpuTextureOptions): true;
        /** Read a persistent texture back as an Image. */
        textureRead(name: string, opts?: GpuDeviceOption): Hull.Image;
        /** Non-blocking variants. */
        readonly async: GpuAsync;
    }
    /** GPU compute. */
    export const gpu: GpuModule;
}
