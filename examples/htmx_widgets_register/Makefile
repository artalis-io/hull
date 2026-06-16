.PHONY: dev clean

# One-command UX-test surface for the §1.5.g widget tier.
# `make dev` spins up the app on a local port and prints the URL.
# Use a temp DB so successive runs start fresh; for persistent
# state, point DB= at a real path.

HULL    ?= ../../build/hull
PORT    ?= 8080
DB      ?= $$TMPDIR/hull-htmx-widgets.db

dev:
	@echo "── htmx_widgets_register dev server ────────────────"
	@echo "Open: http://127.0.0.1:$(PORT)/"
	@echo "Walkthrough: see ./README.md § UX-test checklist"
	@echo
	$(HULL) -p $(PORT) -d $(DB) ./app.lua

clean:
	@rm -f $$TMPDIR/hull-htmx-widgets.db
	@echo "removed temp db"
