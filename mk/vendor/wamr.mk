# mk/vendor/wamr.mk - WAMR (WebAssembly Micro Runtime) vendored runtime.
# The whole HL_ENABLE_WASM ifeq block: the WASM enablement + heap/stack/IO cap
# CFLAGS AND the WAMR vendor config (sources, per-OS platform_init/arch-reloc/
# mremap wiring, objects, AOT invoke) - they share ONE ifeq so they move as a
# unit. WAMR-local platform conditionals (darwin/linux/cosmo) stay inline.
# Extracted verbatim, included at the original position (CFLAGS += order held).

# ── WAMR (WebAssembly Micro Runtime — compute-only) ──────────────
#
# ── WASM (WAMR) ───────────────────────────────────────────────────────
# Optional 7th vendored C library. Provides near-native WASM execution
# for CPU-intensive compute plugins. No WASI, no I/O.
# Disable with: make HL_ENABLE_WASM=0

HL_ENABLE_WASM ?= 1

ifeq ($(HL_ENABLE_WASM),1)
CFLAGS += -DHL_ENABLE_WASM

# User-configurable WASM ceilings (override: make HL_WASM_MAX_HEAP_MB=512)
ifneq ($(HL_WASM_MAX_HEAP_MB),)
CFLAGS += '-DHL_WASM_MAX_HEAP=((uint32_t)$(HL_WASM_MAX_HEAP_MB)*1024*1024)'
endif
ifneq ($(HL_WASM_MAX_STACK_MB),)
CFLAGS += '-DHL_WASM_MAX_STACK=((uint32_t)$(HL_WASM_MAX_STACK_MB)*1024*1024)'
endif
ifneq ($(HL_WASM_MAX_IO_MB),)
CFLAGS += '-DHL_WASM_MAX_IO_SIZE=((uint32_t)$(HL_WASM_MAX_IO_MB)*1024*1024)'
endif

WAMR_DIR     := $(VENDDIR)/wamr
WAMR_CORE    := $(WAMR_DIR)/core
WAMR_IWASM   := $(WAMR_CORE)/iwasm
WAMR_SHARED  := $(WAMR_CORE)/shared

# Platform-specific arch reloc + platform_init
ifeq ($(UNAME_S),Darwin)
WAMR_PLATFORM_INIT := $(WAMR_SHARED)/platform/darwin/platform_init.c
WAMR_PLATFORM_HDR  := $(WAMR_SHARED)/platform/darwin
WAMR_ARCH_RELOC    := $(WAMR_IWASM)/aot/arch/aot_reloc_aarch64.c
WAMR_ARCH_DEFS     := -DBUILD_TARGET=\"AARCH64\" -DWASM_HAVE_MREMAP=0
WAMR_MREMAP_SRC    := $(WAMR_SHARED)/platform/common/memory/mremap.c
  ifeq ($(shell uname -m),x86_64)
    WAMR_ARCH_RELOC := $(WAMR_IWASM)/aot/arch/aot_reloc_x86_64.c
    WAMR_ARCH_DEFS  := -DWASM_HAVE_MREMAP=0
  endif
else
WAMR_PLATFORM_INIT := $(WAMR_SHARED)/platform/linux/platform_init.c
WAMR_PLATFORM_HDR  := $(WAMR_SHARED)/platform/linux
WAMR_ARCH_RELOC    := $(WAMR_IWASM)/aot/arch/aot_reloc_x86_64.c
WAMR_ARCH_DEFS     := -DWASM_HAVE_MREMAP=1 -D_GNU_SOURCE
WAMR_MREMAP_SRC    :=
  ifeq ($(shell uname -m),aarch64)
    WAMR_ARCH_RELOC := $(WAMR_IWASM)/aot/arch/aot_reloc_aarch64.c
    WAMR_ARCH_DEFS  += -DBUILD_TARGET=\"AARCH64\"
  endif
endif

ifdef COSMO
# Cosmopolitan: use WAMR's native cosmo platform support
WAMR_PLATFORM_INIT := $(WAMR_SHARED)/platform/cosmopolitan/platform_init.c
WAMR_PLATFORM_HDR  := $(WAMR_SHARED)/platform/cosmopolitan
WAMR_ARCH_RELOC    := $(WAMR_IWASM)/aot/arch/aot_reloc_x86_64.c
WAMR_ARCH_DEFS     := -DWASM_HAVE_MREMAP=0
WAMR_MREMAP_SRC    := $(WAMR_SHARED)/platform/common/memory/mremap.c
endif

WAMR_SRCS := \
	$(WAMR_IWASM)/common/wasm_application.c \
	$(WAMR_IWASM)/common/wasm_blocking_op.c \
	$(WAMR_IWASM)/common/wasm_c_api.c \
	$(WAMR_IWASM)/common/wasm_exec_env.c \
	$(WAMR_IWASM)/common/wasm_loader_common.c \
	$(WAMR_IWASM)/common/wasm_memory.c \
	$(WAMR_IWASM)/common/wasm_native.c \
	$(WAMR_IWASM)/common/wasm_runtime_common.c \
	$(WAMR_IWASM)/common/wasm_shared_memory.c \
	$(WAMR_IWASM)/interpreter/wasm_interp_fast.c \
	$(WAMR_IWASM)/interpreter/wasm_loader.c \
	$(WAMR_IWASM)/interpreter/wasm_runtime.c \
	$(WAMR_IWASM)/aot/aot_intrinsic.c \
	$(WAMR_IWASM)/aot/aot_loader.c \
	$(WAMR_IWASM)/aot/aot_runtime.c \
	$(WAMR_ARCH_RELOC) \
	$(WAMR_SHARED)/platform/common/posix/posix_blocking_op.c \
	$(WAMR_SHARED)/platform/common/posix/posix_malloc.c \
	$(WAMR_SHARED)/platform/common/posix/posix_memmap.c \
	$(WAMR_SHARED)/platform/common/posix/posix_sleep.c \
	$(WAMR_SHARED)/platform/common/posix/posix_thread.c \
	$(WAMR_SHARED)/platform/common/posix/posix_time.c \
	$(WAMR_PLATFORM_INIT) \
	$(WAMR_SHARED)/mem-alloc/mem_alloc.c \
	$(WAMR_SHARED)/mem-alloc/ems/ems_alloc.c \
	$(WAMR_SHARED)/mem-alloc/ems/ems_gc.c \
	$(WAMR_SHARED)/mem-alloc/ems/ems_hmu.c \
	$(WAMR_SHARED)/mem-alloc/ems/ems_kfc.c \
	$(WAMR_MREMAP_SRC) \
	$(WAMR_SHARED)/utils/bh_assert.c \
	$(WAMR_SHARED)/utils/bh_bitmap.c \
	$(WAMR_SHARED)/utils/bh_common.c \
	$(WAMR_SHARED)/utils/bh_hashmap.c \
	$(WAMR_SHARED)/utils/bh_leb128.c \
	$(WAMR_SHARED)/utils/bh_list.c \
	$(WAMR_SHARED)/utils/bh_log.c \
	$(WAMR_SHARED)/utils/bh_queue.c \
	$(WAMR_SHARED)/utils/bh_vector.c \
	$(WAMR_SHARED)/utils/runtime_timer.c \
	$(WAMR_IWASM)/libraries/shared-heap/shared_heap_wrapper.c

# Flatten WAMR paths to build/ (replace / with _ in the subpath)
WAMR_OBJS := $(patsubst $(WAMR_DIR)/%.c,$(BUILDDIR)/wamr_%.o,$(WAMR_SRCS))

# Native invoker: 64-bit platforms require platform-specific assembly invokers.
# The generic C invoker (invokeNative_general.c) only works on 32-bit platforms
# because it passes argv[] as uint32 values, but 64-bit wasm_runtime_invoke_native
# packs arguments into uint64 slots. Using the generic invoker on 64-bit causes
# argument mangling (pointer split across two params, shifted integer args).
# When SIMD is enabled, float register slots are 128-bit (v128), so the assembly
# invoker must use the _simd variant to match the buffer layout.
WAMR_INVOKE_OBJ := $(BUILDDIR)/wamr_invoke_native.o
# Target-compiler checks must come BEFORE host-platform (UNAME_S=Darwin),
# otherwise cross-building cosmo on macOS picks invokeNative_osx_universal.s
# (Mach-O) for a cosmo (ELF/APE) link and the linker errors with
# "missing elf symbol table". §3.1 of roadmap_next.md.
ifeq ($(notdir $(CC)),cosmocc)
  # Cosmopolitan fat binary: need arch-specific assembly invokers for both
  # x86_64 and aarch64. The generic C invoker mangles 64-bit arguments.
  # cosmocc links two architectures; we compile each with the arch-specific
  # compiler and place the aarch64 object in build/.aarch64/ per convention.
  WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_em64_simd.s
  WAMR_INVOKE_SRC_ARM64 := $(WAMR_IWASM)/common/arch/invokeNative_aarch64_simd.s
  WAMR_INVOKE_OBJ_ARM64 := $(BUILDDIR)/.aarch64/wamr_invoke_native.o
else ifneq ($(findstring x86_64-unknown-cosmo,$(CC)),)
  WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_em64_simd.s
else ifneq ($(findstring aarch64-unknown-cosmo,$(CC)),)
  WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_aarch64_simd.s
else ifeq ($(UNAME_S),Darwin)
  WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_osx_universal.s
  WAMR_INVOKE_FLAGS := -DBH_PLATFORM_DARWIN -DWASM_ENABLE_SIMD=1
else ifeq ($(shell uname -m),x86_64)
  WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_em64_simd.s
else ifeq ($(shell uname -m),aarch64)
  WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_aarch64_simd.s
else
  WAMR_INVOKE_SRC := $(WAMR_IWASM)/common/arch/invokeNative_general.c
endif
WAMR_OBJS += $(WAMR_INVOKE_OBJ)

WAMR_CFLAGS := -std=c11 -O2 -w $(WAMR_ARCH_DEFS) \
	-DWASM_ENABLE_INTERP=1 \
	-DWASM_ENABLE_FAST_INTERP=1 \
	-DWASM_ENABLE_AOT=1 \
	-DWASM_ENABLE_WASI=0 \
	-DWASM_ENABLE_MULTI_MODULE=0 \
	-DWASM_ENABLE_THREAD_MGR=0 \
	-DWASM_ENABLE_LIBC_BUILTIN=0 \
	-DWASM_ENABLE_LIBC_WASI=0 \
	-DWASM_ENABLE_BULK_MEMORY=1 \
	-DWASM_ENABLE_BULK_MEMORY_OPT=1 \
	-DWASM_ENABLE_REF_TYPES=0 \
	-DWASM_ENABLE_SIMD=1 \
	-DWASM_ENABLE_MINI_LOADER=0 \
	-DWASM_ENABLE_SHARED_MEMORY=0 \
	-DWASM_ENABLE_SHARED_HEAP=1 \
	-DWASM_ENABLE_MEMORY64=1 \
	-DWASM_ENABLE_MEMORY_PROFILING=0 \
	-DWASM_ENABLE_MEMORY_TRACING=0 \
	-DWASM_ENABLE_PERF_PROFILING=0 \
	-DWASM_ENABLE_GC=0 \
	-DWASM_ENABLE_STRINGREF=0 \
	-DWASM_ENABLE_EXCE_HANDLING=0 \
	-DWASM_ENABLE_TAGS=0 \
	-DWASM_ENABLE_INSTRUCTION_METERING=1 \
	-DBH_MALLOC=wasm_runtime_malloc \
	-DBH_FREE=wasm_runtime_free \
	-I$(WAMR_IWASM)/include \
	-I$(WAMR_IWASM)/common \
	-I$(WAMR_IWASM)/interpreter \
	-I$(WAMR_IWASM)/aot \
	-I$(WAMR_SHARED)/include \
	-I$(WAMR_SHARED)/platform/include \
	-I$(WAMR_SHARED)/platform/common/posix \
	-I$(WAMR_PLATFORM_HDR) \
	-I$(WAMR_SHARED)/utils \
	-I$(WAMR_SHARED)/mem-alloc \
	-I$(WAMR_CORE)

# WAMR include path for hull source
WAMR_INC := -I$(WAMR_IWASM)/include

WAMR_CFLAGS += $(DEPFLAGS)

else
# WASM disabled
WAMR_OBJS :=
WAMR_INC  :=
endif
