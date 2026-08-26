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

#endif /* HL_LIMITS_GPU_H */
