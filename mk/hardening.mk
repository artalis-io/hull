# mk/hardening.mk - compiler/linker hardening probe layer (docs/security.md 4c).
#
# The whole ifndef-COSMO hardening block: the hl_have_cflag / hl_have_ldflag
# parse-time probes + HARDEN_CFLAGS / HARDEN_LDFLAGS applied to CFLAGS/LDFLAGS.
# Extracted verbatim from the root Makefile (build modularization),
# included at the original position so CFLAGS/LDFLAGS accumulation order holds
# (CFLAGS := / LDFLAGS := are set just above; the block adds to them). Self-
# contained: hl_have_* + comma are defined and used only inside this block.

ifndef COSMO
  # ── Compiler/linker hardening ───────────────────────────────────────
  #
  # Goal: maximise practical ROP/JOP resistance using whatever the host
  # toolchain supports, without breaking portability. Strategy:
  #
  #   1. Always-on, baseline-portable flags applied unconditionally.
  #   2. Probe-and-add flags that newer compilers/linkers accept but
  #      older ones reject — see hl_have_cflag / hl_have_ldflag below.
  #   3. Skip the whole layer if HULL_DISABLE_HARDENING=1 (debug only;
  #      do not ship release binaries with this unset).
  #
  # The probes write a tiny program to a tmpfile and discard the result;
  # they run once at Makefile-parse time. -Werror upgrades warnings (the
  # macOS clang "argument unused during compilation" class) to errors so
  # flags that are accepted-with-warning are correctly rejected.
  #
  # Cosmocc is excluded from this entire block — APE format constraints
  # mean ELF-specific options (PIE, RELRO, CET notes) are inapplicable
  # or break the linker script.
  ifndef HULL_DISABLE_HARDENING
    # Baseline: stack canaries + PIE. PIE is the macOS default since
    # 10.7 — passing `-pie` to clang on Darwin emits the
    # "argument unused during compilation" warning that pollutes every
    # link line. Only set the linker side where it actually matters.
    CFLAGS  += -fstack-protector-strong -fPIE
    ifneq ($(UNAME_S),Darwin)
      LDFLAGS += -pie
    endif

    ifeq ($(DEBUG)$(TSAN),)
      # _FORTIFY_SOURCE=3 requires glibc 2.34+ / gcc 12+ / clang 9+; on
      # older toolchains it emits a noisy warning and behaves as =2.
      # We intentionally leave the warning loud so stale CI is visible.
      # Skipped under sanitizer builds (DEBUG/TSAN) which use -O0/-O1.
      CFLAGS += -D_FORTIFY_SOURCE=3
    endif

    # Probe macros. Echo the flag if accepted, empty otherwise.
    # Use $(comma) inside the argument so calls like
    # $(call hl_have_ldflag,-Wl$(comma)--as-needed) work — `$(call X,a,b)`
    # would otherwise see two arguments split on the literal comma.
    comma := ,
    hl_have_cflag = $(shell tmp="$$(mktemp 2>/dev/null || echo /tmp/hlprobe$$$$.o)"; \
        printf 'int main(void){return 0;}\n' \
        | $(CC) -Werror $(1) -x c -c -o "$$tmp" - >/dev/null 2>&1 \
        && echo "$(1)"; rm -f "$$tmp")
    hl_have_ldflag = $(shell tmp="$$(mktemp 2>/dev/null || echo /tmp/hlprobe$$$$)"; \
        printf 'int main(void){return 0;}\n' \
        | $(CC) -x c - -o "$$tmp" $(1) >/dev/null 2>&1 \
        && echo "$(1)"; rm -f "$$tmp")

    # Universal CFLAGS. Each one is independently probed because old
    # toolchains, ld variants, and Apple clang reject different subsets.
    #
    #  -fstack-clash-protection
    #      gcc 8+ / clang 11+. Inserts a probe per stack frame >4K so
    #      a large alloca can't jump the guard page and pivot the stack.
    #  -fno-plt
    #      Direct GOT calls instead of trampolining through the PLT.
    #      Shrinks ROP gadget surface and lets RELRO+BIND_NOW eliminate
    #      every writable function pointer.
    #  -fno-common
    #      Reject K&R-style tentative definitions (default in gcc 10+/
    #      clang 11+; explicit here to lock behaviour on older
    #      toolchains).
    #  -ftrivial-auto-var-init=zero
    #      Zero-initialise stack vars (clang 8+ / gcc 12+). Mitigates
    #      info-leak primitives from uninitialised reads.
    HARDEN_CFLAGS := \
        $(call hl_have_cflag,-fstack-clash-protection) \
        $(call hl_have_cflag,-fno-plt) \
        $(call hl_have_cflag,-fno-common) \
        $(call hl_have_cflag,-ftrivial-auto-var-init=zero) \
        $(call hl_have_cflag,-fzero-call-used-regs=used-gpr)
    # -fzero-call-used-regs=used-gpr (gcc 11+ / clang 15+): zero
    # general-purpose registers on function return so a ROP gadget
    # found in our text segment can't inherit useful values from the
    # caller's register state. Cost: ~1-2% binary size, negligible
    # runtime. Probed because older toolchains reject the flag.

    # Architecture-specific CFI / branch hardening.
    #
    #  x86_64: -fcf-protection=full
    #      Emits Intel CET markers (ENDBR for IBT + shadow-stack note).
    #      Generated on any x86_64 build; CPUs without CET ignore the
    #      NOPs. Linux kernels 5.18+ enforce when supported. Apple clang
    #      accepts the flag silently.
    #  arm64: -mbranch-protection=standard
    #      Equivalent: pac-ret (signed return addresses) + BTI. clang
    #      14+/gcc 9+. macOS arm64 accepts and emits the instructions;
    #      enforcement is up to the kernel/runtime.
    ifneq (,$(filter x86_64 amd64,$(UNAME_M)))
      HARDEN_CFLAGS += $(call hl_have_cflag,-fcf-protection=full)
    endif
    ifneq (,$(filter arm64 aarch64,$(UNAME_M)))
      HARDEN_CFLAGS += $(call hl_have_cflag,-mbranch-protection=standard)
    endif

    CFLAGS += $(HARDEN_CFLAGS)

    # Linker hardening. Linux-only -z options are still gated by host
    # OS — ld64 (macOS) rejects them. Some are universal.
    ifeq ($(UNAME_S),Linux)
      CFLAGS  += -D_DEFAULT_SOURCE
      LDFLAGS += -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
      # -Wl,-z,separate-code: separates code/data pages so a write
      #  primitive on a writable page cannot land in executable
      #  memory by accident. GNU ld 2.30+ / lld.
      HARDEN_LDFLAGS := $(call hl_have_ldflag,-Wl$(comma)-z$(comma)separate-code)
      LDFLAGS += $(HARDEN_LDFLAGS)
    endif
    # -Wl,--as-needed drops unused DT_NEEDED entries; shrinks the
    # loaded-library surface for ROP gadget hunters. Universal on
    # GNU ld / gold / lld; ld64 ignores it harmlessly.
    LDFLAGS += $(call hl_have_ldflag,-Wl$(comma)--as-needed)
  endif

  # (Earlier audit rounds added `-Wl,--build-id=none` here under the
  # wrong theory that Linux Build-ID was random. Reality: GNU ld's
  # default `--build-id=sha1` is content-addressed — identical inputs
  # produce identical Build-IDs. Keeping the default preserves the
  # Build-ID for debuggers/crash reporters without sacrificing
  # reproducibility. Same applies to macOS LC_UUID: deterministic given
  # same output path + same input content. See roadmap_next.md §0.2 for
  # the full investigation arc.)
endif
