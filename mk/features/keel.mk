# mk/features/keel.mk - Keel event-loop composable feature (docs/keel_feature.md).
#
# The Keel EVENT LOOP archive (libhull_feature-keel.a: serve.o KlServer loop +
# async_keel + net_keel + the server-only static/agent/test objects) + its SLIM-
# base embed (EMBEDDED_KEEL_H, under HL_APP_BASE_SQLITELESS+TLSLESS). Moved
# verbatim from two regions of the root Makefile (build modularization, Phase 1).
#
# Stays in the root Makefile: the base-entry gating - SERVE_OBJ (serve.o vs the
# Keel-free serve_cli.o under HL_KEEL_FEATURE), APP_RUNNER_OBJ, the serve_cli.o /
# app_runner.o compile rules, and the platform-keelless self-verify target -
# which control what the BASE links, a base concern. (Phase 2 macro-ifies the
# archive rule.)

# libhull_feature-keel.a: the Keel EVENT LOOP as a composable feature
# (docs/keel_feature.md, Phase 4.2b). The event-loop objects a Keel-less base
# (HL_KEEL_FEATURE=1) drops -- serve.o (the KlServer serve loop + strong
# hull_serve), async_keel.o (strong hl_async_backend), net_keel.o (strong
# hl_net_op_*), plus the server-only hull_static.o / agent_api.o / test_runner.o.
# Composed (whole-archived) at `hull build` on needs_http, but ONLY onto a
# Keel-less base (build.lua nm-probes for an ABSENT strong hull_serve, so a full
# base -- which already carries these -- never double-composes them). Built from
# the default (HTTP_SERVER=1, HL_KEEL_FEATURE=0) objects, i.e. the real server.
FEATURE_KEEL_OBJS := $(BUILDDIR)/serve.o $(BUILDDIR)/async_keel.o \
                     $(BUILDDIR)/net_keel.o $(BUILDDIR)/hull_static.o \
                     $(BUILDDIR)/agent_api.o $(BUILDDIR)/test_runner.o
feature-keel: $(BUILDDIR)/libhull_feature-keel.a
.PHONY: feature-keel
$(BUILDDIR)/libhull_feature-keel.a: $(FEATURE_KEEL_OBJS) | $(BUILDDIR)
	$(call AR_FEATURE_LIB,$(FEATURE_KEEL_OBJS))

# Keel feature archive embed (Phase 4.2b): keel is folded into the SLIM base, so
# when the app-build base is SLIM (SQLITELESS + TLSLESS both set) it is also
# Keel-less -- embed libhull_feature-keel.a (serve.o + async_keel + net_keel +
# the server-only static/agent/test objects) so an http app auto-composes the
# Keel event loop with no `hull feature install`. Only pulled for the SLIM base.
ifeq ($(HL_APP_BASE_SQLITELESS)$(HL_APP_BASE_TLSLESS),11)
EMBEDDED_KEEL_H := $(BUILDDIR)/embedded_keel.h
$(EMBEDDED_KEEL_H): $(BUILDDIR)/libhull_feature-keel.a | $(BUILDDIR)
	@echo "/* Auto-generated - do not edit */" > $@
	@xxd -i $(BUILDDIR)/libhull_feature-keel.a | sed 's/build_libhull_feature_keel_a/hl_embedded_feature_keel_a/g' | $(XXD_CONST_PIPE) >> $@
CFLAGS += -DHL_BUILD_EMBEDDED_KEEL
$(BUILD_ASSET_OBJ): $(EMBEDDED_KEEL_H)
endif
