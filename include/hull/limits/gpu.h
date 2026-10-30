/*
 * hull/limits/gpu.h - GPU compute (wgpu-native) limits
 *
 * Used only by cap/gpu sources, worker_gpu, runtime/{lua,js}/mod_gpu,
 * and manifest.{h,c} (for HL_GPU_MAX_DEVICES). Changes here should not
 * force a rebuild of non-GPU TUs.
 *
 * All maximums are #ifndef-guarded so Makefile -D can override them.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_LIMITS_GPU_H
#define HL_LIMITS_GPU_H

#ifndef HL_GPU_MAX_DEVICES
#define HL_GPU_MAX_DEVICES       8
#endif
#ifndef HL_GPU_PIPELINE_MAX
#define HL_GPU_PIPELINE_MAX      64
#endif
#ifndef HL_GPU_BUFFER_MAX
#define HL_GPU_BUFFER_MAX        256
#endif
#ifndef HL_GPU_NAME_MAX
#define HL_GPU_NAME_MAX          256
#endif
#ifndef HL_GPU_MAX_DISPATCH_SIZE
#define HL_GPU_MAX_DISPATCH_SIZE (256 * 1024 * 1024)  /* 256 MB */
#endif
#ifndef HL_GPU_MAX_PIPELINE_STAGES
#define HL_GPU_MAX_PIPELINE_STAGES  16
#endif
#ifndef HL_GPU_MAX_PIPELINE_BUFFERS
#define HL_GPU_MAX_PIPELINE_BUFFERS 64  /* total across all stages */
#endif
#ifndef HL_GPU_MAX_PIPELINE_OUTPUTS
#define HL_GPU_MAX_PIPELINE_OUTPUTS 8
#endif

#ifndef HL_GPU_TEXTURE_MAX
#define HL_GPU_TEXTURE_MAX       64
#endif
#ifndef HL_GPU_MAX_TEXTURE_DIM
#define HL_GPU_MAX_TEXTURE_DIM   16384
#endif

/* Bytes the persistent buffers and textures of one device may hold between
 * them (audit 17). A create past it is refused before the backend runs. */
#ifndef HL_GPU_MAX_LIVE_BYTES
#define HL_GPU_MAX_LIVE_BYTES    ((size_t)1 << 30)   /* 1 GiB */
#endif

/* Bytes of buffer / texture writes one device may hold queued (their
 * staging copies, which the backend keeps until a submission completes)
 * before the cap layer has the backend submit and wait for them
 * (audit 34). At most this plus one write is ever outstanding. */
#ifndef HL_GPU_MAX_QUEUED_WRITE_BYTES
#define HL_GPU_MAX_QUEUED_WRITE_BYTES ((size_t)64 << 20)   /* 64 MiB */
#endif

/* The largest WGSL source gpu.compile takes or gpu.load reads (audit 36).
 * Real compute shaders are a few KiB; the largest shipped is under 4 KiB. */
#ifndef HL_GPU_WGSL_MAX_BYTES
#define HL_GPU_WGSL_MAX_BYTES    ((size_t)1 << 20)   /* 1 MiB */
#endif

/* The deepest expression a shader may build, as hl_gpu_wgsl_complexity
 * estimates it: the operators and accessors chained in one operand (a
 * comma-separated argument restarts the count) plus the brackets open
 * around it, within one statement (audit 36). naga (wgpu-native's WGSL
 * compiler) parses a left-associative chain - `a + a + ...`, `v.wzyx.wzyx...`,
 * `x == 1u && x == 1u && ...` - into a tree as deep as the chain and walks
 * it recursively in validation and the backend writers; on the event
 * loop's 8 MiB stack the process died (SIGSEGV) at ~9000 chained `+`,
 * ~10000 swizzles, ~5000 `&&` terms. Hand-written and generated shaders put
 * tens, rarely low hundreds, of operators in one operand: 1000 is far above
 * them and ~8x below the crash. */
#ifndef HL_GPU_WGSL_EXPR_DEPTH_MAX
#define HL_GPU_WGSL_EXPR_DEPTH_MAX 1000
#endif

/* The deepest block nesting a shader may build, braces and `else if`
 * chains together, as hl_gpu_wgsl_block_depth counts it (audit 37). naga's
 * parser stops at 127 nested braces, but an `else if` nests without one
 * and naga validates the resulting If tree recursively: on the event
 * loop's 8 MiB stack a flat chain of 5400 `else if` links killed the
 * process (SIGSEGV in validate_block_impl; 5300 survived, Apple M1 Max /
 * Metal). Real shaders nest a handful of blocks and chain tens of
 * `else if`; 512 is ~10x below the crash, and Metal itself refuses more
 * than 256 nested brackets at pipeline creation. */
#ifndef HL_GPU_WGSL_BLOCK_DEPTH_MAX
#define HL_GPU_WGSL_BLOCK_DEPTH_MAX 512
#endif

/* The deepest type a shader's module-scope `struct` / `alias`
 * declarations may build, as hl_gpu_wgsl_type_depth counts it (audit 37):
 * a chain `alias A1 = array<A0, 1>; alias A2 = array<A1, 1>; ...` is
 * expanded in full by naga and the backend writers, work that grows with
 * the square of its length - 5000 links took 6 s and 10000 took 24 s on
 * the event loop, uncharged, and a few hundred crashed Metal's compiler
 * service (pipeline creation then fails). Real shaders nest types a few
 * levels deep; 64 keeps the work negligible and is ~4x below where the
 * Metal compiler first failed (between 200 and 300 aliases). */
#ifndef HL_GPU_WGSL_TYPE_DEPTH_MAX
#define HL_GPU_WGSL_TYPE_DEPTH_MAX 64
#endif

#endif /* HL_LIMITS_GPU_H */
