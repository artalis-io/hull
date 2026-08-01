# mk/features/http.mk - HTTP composable feature (issue #114, docs/http_feature_phase1.md).
#
# The runtime-agnostic HTTP CORE (libhull_feature-http.a: cap/http+async, ws,
# smtp, body) + the per-runtime web BINDINGS (routes/res/ws/sse/... ->
# libhull_feature-http-<rt>.a) + the embed. Moved verbatim from five regions of
# the root Makefile (build modularization, Phase 1). Included EARLY: before
# PLATFORM_CAP_OBJS filters FEATURE_HTTP_OBJS, and before FEATURE_LUA/JS_OBJS
# filter out FEATURE_HTTP_LUA/JS_OBJS.
#
# Stays in the root Makefile: HTTP_WEAKSTUB_OBJ (base weak stub), the
# PLATFORM_CAP_OBJS + FEATURE_LUA/JS_OBJS filters (which reference these vars),
# the RUNTIME_FEATURE_LIBS refs, and the shared CFLAGS/BUILD_ASSET_OBJ lines.
# Keel (the event loop) is a separate concern -> mk/features/keel.mk. (Phase 2
# macro-ifies the archive rules.)

# The runtime-agnostic HTTP core caps (cap/http + async, ws, smtp, body). Defined
# here (before PLATFORM_OBJS) so the native app base can filter them out - they
# move into libhull_feature-http.a (issue #114, Phase B) and are composed back at
# `hull build`. The feature-http archive target below reuses this list.
FEATURE_HTTP_OBJS := $(BUILDDIR)/cap_http.o $(BUILDDIR)/cap_http_async.o \
                     $(BUILDDIR)/cap_ws.o $(BUILDDIR)/cap_smtp.o $(BUILDDIR)/cap_body.o

# libhull_feature-http.a: the runtime-agnostic HTTP CORE as a composable feature
# archive (issue #114, Phase B). Bundles the HTTP capability objects - client
# (cap/http + async), server WebSocket registry (cap/ws), SMTP (cap/smtp), and
# the request body reader (cap/body). These are runtime-agnostic C, reached by
# both runtimes through the cap layer. Phase A already routed the runtime core
# through the hl_*_http_* seam, so this archive holds the strong seam overrides
# that live cap-side (hl_http_ws_registry_free in cap/ws.o). This step is
# ADDITIVE (base still compiles HTTP in); a later step makes the base
# HTTP-core-less and composes this. The per-runtime web BINDINGS (routes, sse,
# ws, bindings_response, http_register, mod_http_*/ws_*/sse/smtp/request) are a
# separate per-runtime archive (Phase C). Keel is linked separately (the app
# still pulls libkeel.a), not bundled here.
# (FEATURE_HTTP_OBJS is defined earlier, above PLATFORM_OBJS, so the native app
# base filters these caps out; the archive below and the base share one list.)

$(eval $(call define-feature-archive,http,$(FEATURE_HTTP_OBJS)))

# Per-runtime web bindings (issue #114, Phase C). This is the same set the
# HL_ENABLE_HTTP_SERVER=0 + HL_ENABLE_HTTP_CLIENT=0 source filters enumerate -
# the tested definition of "what is HTTP" per runtime. They move OUT of the pure
# runtime archive into libhull_feature-http-<rt>.a, composed only when an app
# needs HTTP; the pure runtime references a few of their symbols and the base
# http_weakstub.o satisfies that link when the web archive is not composed.
FEATURE_HTTP_RT_NAMES := mod_ws_server mod_ws_client mod_http_server mod_sse \
    mod_test routes dispatch sse ws timers mod_request bindings bindings_response \
    http_register mod_http_client mod_smtp
FEATURE_HTTP_LUA_OBJS := $(addprefix $(BUILDDIR)/lua_rt_,$(addsuffix .o,$(FEATURE_HTTP_RT_NAMES)))
FEATURE_HTTP_JS_OBJS  := $(addprefix $(BUILDDIR)/js_,$(addsuffix .o,$(FEATURE_HTTP_RT_NAMES)))

# Per-runtime web-bindings feature archives (issue #114, Phase C).
$(eval $(call define-feature-archive,http-lua,$(FEATURE_HTTP_LUA_OBJS)))

$(eval $(call define-feature-archive,http-js,$(FEATURE_HTTP_JS_OBJS)))

# Embed the HTTP core feature archive too (issue #114). The native base is
# HTTP-core-less; the default distributed hull composes this back for every
# full-flavor app with no `hull feature install`. (Cosmo keeps HTTP in the base,
# so this native single-arch embed path is the only one that needs it.)
#
# The per-runtime web bindings (Phase C) are embedded here too, so a full-flavor
# app composes the http core + its runtime's web bindings with no install.
EMBEDDED_HTTP_H := $(BUILDDIR)/embedded_http.h
$(EMBEDDED_HTTP_H): $(BUILDDIR)/libhull_feature-http.a $(BUILDDIR)/libhull_feature-http-lua.a $(BUILDDIR)/libhull_feature-http-js.a | $(BUILDDIR)
	@echo "/* Auto-generated - do not edit */" > $@
	@xxd -i $(BUILDDIR)/libhull_feature-http.a     | sed 's/build_libhull_feature_http_a/hl_embedded_feature_http_a/g'         | $(XXD_CONST_PIPE) >> $@
	@xxd -i $(BUILDDIR)/libhull_feature-http-lua.a | sed 's/build_libhull_feature_http_lua_a/hl_embedded_feature_http_lua_a/g' | $(XXD_CONST_PIPE) >> $@
	@xxd -i $(BUILDDIR)/libhull_feature-http-js.a  | sed 's/build_libhull_feature_http_js_a/hl_embedded_feature_http_js_a/g'   | $(XXD_CONST_PIPE) >> $@
