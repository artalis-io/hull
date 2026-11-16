# mk/js_frontend_dump.mk - the JS-mode frontend snapshot tools (test-only, never shipped).
#
# build/js_frontend_dump dumps lex / parse / resolveScope output of a source tree; the
# golden test (tests/hull/frontend/test_js_golden.c) shares its core. Self-contained so
# scripts/js_frontend_diff.sh can add it to an older checkout with `make -f Makefile -f
# mk/js_frontend_dump.mk` when that checkout's Makefile does not include it yet.

$(BUILDDIR)/js_frontend_dump: $(TESTDIR)/hull/frontend/js_frontend_dump.c $(TESTDIR)/hull/frontend/js_frontend_dump.h $(FRONTEND_JS_SESSION_OBJ) $(STDLIB_JS_CLI_REGISTRY_O) $(QJS_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -Ivendor/quickjs -o $@ $< $(FRONTEND_JS_SESSION_OBJ) $(STDLIB_JS_CLI_REGISTRY_O) $(QJS_OBJS) -lm -lpthread $(LDFLAGS)

# Regenerate tests/fixtures/js_frontend_golden/golden.txt. Only for a MEANT change to
# JS-mode output: say why in the PR, the golden diff is then visible in review.
.PHONY: js-frontend-golden
js-frontend-golden: $(BUILDDIR)/test_js_golden
	HULL_JS_GOLDEN_UPDATE=1 $(BUILDDIR)/test_js_golden --filter='js_golden.snapshot_matches'

# Compare JS-mode output over the repo JS between the merge base and this checkout
# (BASE=<ref> to choose another base).
.PHONY: js-frontend-diff
js-frontend-diff:
	sh scripts/js_frontend_diff.sh $(BASE)
