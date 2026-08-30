# Hull landing page
#
# Builds the deployable site into build/. `aws s3 sync` does not follow
# symlinks by default, and site/install.sh is a symlink to the canonical
# install.sh at the repo root - so we dereference with `cp -L` so the
# deployed install.sh is a real file with the current contents.
#
# Mirrors the otto site Makefile pattern so secrets / workflow steps
# stay consistent across artalis-io repos.

.PHONY: all build clean serve

BUILD_DIR := build

# Files that ship in the deployed site. install.sh is the symlink that
# must be dereferenced; everything else is a regular file.
STATIC_FILES := index.html verify.html mark.svg og-card.svg og-card.png
# Dereferenced symlinks to canonical repo-root files: install.sh plus its
# minisign signature + public key, so `curl gethull.dev/install.sh{,.minisig}`
# and the offline "verify the installer" path both work; and install.ps1 so
# `irm https://gethull.dev/install.ps1 | iex` serves the current Windows
# installer.
SYMLINKED    := install.sh install.sh.minisig minisign.pub install.ps1
# Subdirs: vendored third-party assets (Tailwind, Lucide) and fonts.
# Self-hosted so the "no third-party CDN" claim holds for the site too.
SUBDIRS      := vendor fonts

all: build

build:
	@mkdir -p $(BUILD_DIR)
	@cp $(STATIC_FILES) $(BUILD_DIR)/
	@cp -L $(SYMLINKED) $(BUILD_DIR)/
	@chmod 0644 $(BUILD_DIR)/install.sh
	@for d in $(SUBDIRS); do mkdir -p $(BUILD_DIR)/$$d && cp $$d/* $(BUILD_DIR)/$$d/; done
	@echo "Site built in $(BUILD_DIR)/"
	@echo "  Files: $$(find $(BUILD_DIR) -type f | wc -l | tr -d ' ')"
	@find $(BUILD_DIR) -type f -exec ls -l {} \;

# Local preview at http://localhost:8000 - useful when iterating on the
# landing page before pushing.
serve: build
	@echo "Serving $(BUILD_DIR)/ at http://localhost:8000"
	@cd $(BUILD_DIR) && python3 -m http.server 8000

clean:
	rm -rf $(BUILD_DIR)
