# mk/features/tui.mk - Terminal UI composable feature (docs/features_and_flavors.md).
#
# The tui FEATURE ARCHIVE (libhull_feature-tui.a: cap_tui + input + width, the
# whole-archived cap core) + the per-runtime bridges (mod_tui ->
# libhull_feature-tui-<rt>.a) + the embed. Moved verbatim from four regions of
# the root Makefile (build modularization, Phase 1).
#
# Stays in the root Makefile: the toolchain force-load (TUI_TOOLCHAIN_ARCHIVE /
# _LDFLAGS + HL_TUI_TOOLCHAIN) which force-loads these archives into the hull
# TOOL, cap_tui_feature.o (base weak hooks), and the AR_FEATURE_LIB define
# (a general macro). (Phase 2 macro-ifies the archive rules.)

# ── TUI feature archive (composable feature: hull build --with=tui) ──
# libhull_feature-tui.a bundles the whole TUI subsystem: the cap layer
# (cap_tui.o + cap_tui_input.o + cap_tui_width.o) and both runtime bridges
# (lua_rt_mod_tui.o + js_mod_tui.o). Those objects carry the STRONG overrides of
# the base's weak feature hooks (hl_tui_feature_present in cap/tui.c;
# hl_tui_feature_register_lua / _js in the mod_tui.c files), so composing the
# archive lights TUI up. Unlike a backend feature (duckdb/gpu) there is no single
# entry symbol, so the compose link must whole-archive / -force_load this lib to
# pull every member (wired in build.lua's FEATURE_SPECS.tui). The archive is
# SELF-CONTAINED: cap_tui_width.o (used only by the tui trio) travels with it so
# a GNU-ld whole-archive compose resolves its refs internally. No vendored lib
# and no extra link libs (TUI is pure POSIX termios). cap_tui_feature.o (weak
# base hooks) stays base-resident.
#
# The five subsystem objects are filtered OUT of a TUI-free base build, so they
# only ever exist to back this archive. The target-specific rule below compiles
# them with -DHL_ENABLE_TUI regardless of the base HL_ENABLE_TUI setting -- tui.h
# is ABI-independent of the flag (its feature hooks are unconditional void*
# signatures), so a TUI-enabled cap_tui.o links cleanly into a TUI-free base.
# This means the archive builds in-place with no sub-make / separate builddir:
# `make` (TUI-off default) can produce both the lean base AND this archive in one
# pass for the toolchain force_load (HL_TUI_TOOLCHAIN).
feature-tui: $(BUILDDIR)/libhull_feature-tui.a
.PHONY: feature-tui

$(BUILDDIR)/cap_tui.o $(BUILDDIR)/cap_tui_input.o $(BUILDDIR)/cap_tui_width.o \
$(BUILDDIR)/lua_rt_mod_tui.o $(BUILDDIR)/js_mod_tui.o: CFLAGS += -DHL_ENABLE_TUI

# libhull_feature-tui.a is the runtime-AGNOSTIC cap core only (issue #114,
# Phase D). The per-runtime bridges (lua_rt_mod_tui.o / js_mod_tui.o) whole-
# archive would pull BOTH runtimes' bridges, and the non-composed runtime's
# bridge references runtime symbols the runtime-less base lacks -> undefined at
# link. So each bridge is its own archive (libhull_feature-tui-<rt>.a), and a
# `--with=tui` app whole-archives the cap core + ONLY its runtime's bridge. The
# cap core stays the single INSTALLABLE asset (FEATURES[]/release unchanged); the
# tiny bridges are embedded in hull and composed from there (like the web
# bindings), so `hull feature install tui` still fetches one archive.
$(BUILDDIR)/libhull_feature-tui.a: $(BUILDDIR)/cap_tui.o $(BUILDDIR)/cap_tui_input.o \
                                   $(BUILDDIR)/cap_tui_width.o | $(BUILDDIR)
	@rm -f $@
	$(AR) rcs $@ $(BUILDDIR)/cap_tui.o $(BUILDDIR)/cap_tui_input.o \
	            $(BUILDDIR)/cap_tui_width.o
	@echo "built $@ ($$(du -h $@ | cut -f1))"

# Per-runtime tui bridge archives (issue #114, Phase D). Tiny (one object each);
# embedded in hull + composed for the app's runtime alongside the cap core.
$(eval $(call define-feature-archive,tui-lua,$(BUILDDIR)/lua_rt_mod_tui.o))

$(eval $(call define-feature-archive,tui-js,$(BUILDDIR)/js_mod_tui.o))

EMBEDDED_TUI_H := $(BUILDDIR)/embedded_tui.h
$(EMBEDDED_TUI_H): $(BUILDDIR)/libhull_feature-tui-lua.a $(BUILDDIR)/libhull_feature-tui-js.a | $(BUILDDIR)
	@echo "/* Auto-generated - do not edit */" > $@
	@xxd -i $(BUILDDIR)/libhull_feature-tui-lua.a | sed 's/build_libhull_feature_tui_lua_a/hl_embedded_feature_tui_lua_a/g' | $(XXD_CONST_PIPE) >> $@
	@xxd -i $(BUILDDIR)/libhull_feature-tui-js.a  | sed 's/build_libhull_feature_tui_js_a/hl_embedded_feature_tui_js_a/g'   | $(XXD_CONST_PIPE) >> $@
