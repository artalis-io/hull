# bench/wasm/workloads/Makefile — Compile C workloads to WASM
#
# Produces .wasm files for the Hull WASM compute benchmark.
# Requires clang with wasm32 target support and wasm-ld.
#
# Usage:
#   make                          # build all .wasm files
#   make WASM_CC=/path/to/clang   # override clang path
#   make aot                      # build AOT files (requires wamrc in PATH or WAMRC=)
#   make clean                    # remove generated .wasm and .aot files
#
# SPDX-License-Identifier: AGPL-3.0-or-later

# ── Toolchain ────────────────────────────────────────────────────────

# Prefer llvm@18 on macOS Homebrew (has wasm-ld bundled)
LLVM18_PREFIX := $(shell brew --prefix llvm@18 2>/dev/null)
ifneq ($(LLVM18_PREFIX),)
  WASM_CC  ?= $(LLVM18_PREFIX)/bin/clang
  WASM_LD  ?= $(LLVM18_PREFIX)/bin/wasm-ld
else
  WASM_CC  ?= clang
  WASM_LD  ?= wasm-ld
endif

WAMRC ?= wamrc

# ── Flags ────────────────────────────────────────────────────────────

WASM_CFLAGS_COMMON := --target=wasm32-unknown-unknown \
	-nostdlib -O2 -flto \
	-Wl,--no-entry \
	-Wl,--export=hull_process \
	-Wl,--export=hull_version \
	-Wl,--export=memory \
	-Wl,--allow-undefined \
	-Wl,--initial-memory=131072 \
	-Wl,--max-memory=67108864

WASM_CFLAGS_SCALAR := $(WASM_CFLAGS_COMMON)
WASM_CFLAGS_SIMD   := $(WASM_CFLAGS_COMMON) -msimd128

# ── Arch detection for AOT ───────────────────────────────────────────

UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),x86_64)
  AOT_ARCH := x86_64
  AOT_TARGET := x86_64
else ifeq ($(UNAME_M),arm64)
  AOT_ARCH := aarch64
  AOT_TARGET := aarch64
else ifeq ($(UNAME_M),aarch64)
  AOT_ARCH := aarch64
  AOT_TARGET := aarch64
else
  AOT_ARCH := $(UNAME_M)
  AOT_TARGET := $(UNAME_M)
endif

# ── Targets ──────────────────────────────────────────────────────────

# SIMD workloads: both scalar (no SIMD) and SIMD variants
SIMD_WORKLOADS := simd_dot_product simd_matmul

SCALAR_WASM := $(addsuffix .wasm,$(SIMD_WORKLOADS))
SIMD_WASM   := $(addsuffix _simd.wasm,$(SIMD_WORKLOADS))

ALL_WASM := $(SCALAR_WASM) $(SIMD_WASM)
ALL_AOT  := $(patsubst %.wasm,%.aot.$(AOT_ARCH),$(ALL_WASM))

.PHONY: all aot clean check

all: $(ALL_WASM)

aot: $(ALL_AOT)

# ── Compile rules ────────────────────────────────────────────────────

# Scalar variant (no SIMD intrinsics)
%.wasm: %.c
	$(WASM_CC) $(WASM_CFLAGS_SCALAR) -o $@ $<

# SIMD variant
%_simd.wasm: %.c
	$(WASM_CC) $(WASM_CFLAGS_SIMD) -o $@ $<

# AOT compilation (from any .wasm)
%.aot.$(AOT_ARCH): %.wasm
	$(WAMRC) --target=$(AOT_TARGET) --enable-simd -o $@ $<

# ── Verify ───────────────────────────────────────────────────────────

check: $(ALL_WASM)
	@for f in $(ALL_WASM); do \
		echo "Checking $$f..."; \
		if file $$f | grep -q "WebAssembly"; then \
			echo "  OK"; \
		else \
			echo "  FAILED: not a valid WASM module"; \
			exit 1; \
		fi; \
	done
	@echo "All WASM modules valid."

# ── Clean ────────────────────────────────────────────────────────────

clean:
	rm -f $(ALL_WASM) $(ALL_AOT)
	rm -f *.aot.*
