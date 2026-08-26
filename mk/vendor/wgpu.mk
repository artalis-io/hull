# mk/vendor/wgpu.mk - wgpu-native GPU backend config (WGPU_LIB + the Metal/
# Vulkan framework selection). Native-only. Extracted verbatim.

# ── wgpu-native (GPU compute - optional) ─────────────────────────
#
# Optional GPU compute backend. Disabled by default.
# Enable with: make HL_ENABLE_GPU=1
#   - Auto-detects vendor/wgpu/libwgpu_native.a if present
#   - Or specify: make HL_ENABLE_GPU=1 WGPU_LIB_DIR=/path/to/lib
#   - Fetch automatically: make fetch-wgpu && make HL_ENABLE_GPU=1

HL_ENABLE_GPU ?= 0

ifeq ($(HL_ENABLE_GPU),1)
  CFLAGS += -DHL_ENABLE_GPU -I$(VENDDIR)/wgpu
  # Auto-detect vendor/wgpu if WGPU_LIB_DIR not specified
  ifndef WGPU_LIB_DIR
    ifneq (,$(wildcard $(VENDDIR)/wgpu/libwgpu_native.a))
      WGPU_LIB_DIR := $(VENDDIR)/wgpu
    else
      $(error HL_ENABLE_GPU=1 requires wgpu-native. Run: make fetch-wgpu)
    endif
  endif
  WGPU_LIB := $(WGPU_LIB_DIR)/libwgpu_native.a
  ifeq ($(UNAME_S),Darwin)
    WGPU_FRAMEWORKS := -framework Metal -framework QuartzCore -framework CoreGraphics -framework Foundation
  else
    WGPU_FRAMEWORKS := -lvulkan
  endif
  # GPU is not compatible with Cosmopolitan builds
  ifdef COSMO
    $(error GPU compute is not compatible with Cosmopolitan builds)
  endif
  WORKER_GPU_OBJ := $(BUILDDIR)/worker_gpu.o
else
  WORKER_GPU_OBJ := $(BUILDDIR)/worker_gpu.o
  WGPU_LIB :=
  WGPU_FRAMEWORKS :=
endif

