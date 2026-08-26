/**
 * @file quickjs_tag.h
 * @brief Single source of truth for the vendored-QuickJS snapshot tag
 *        used by every JS-side bytecode / template cache.
 *
 * `QJS_TAG` is folded into the cache key of every persisted JS
 * artifact (`js-bytecode`, `js-templates`). Any change to the
 * vendored QuickJS - a version bump, a cherry-pick that touches
 * opcode encoding, a struct-layout-altering backport - invalidates
 * every cached artifact written under the old tag.
 *
 * The tag is built from `HL_QJS_VERSION`, which the Makefile injects
 * via `-DHL_QJS_VERSION="<version>"`. The same Makefile variable
 * (`QJS_VERSION`) is also passed to the vendored QuickJS as
 * `CONFIG_VERSION`, so the two are guaranteed to move together.
 * Bumping vendor/quickjs/ is now a one-line edit in the Makefile.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_QUICKJS_TAG_H
#define HL_QUICKJS_TAG_H

#ifndef HL_QJS_VERSION
#error "HL_QJS_VERSION must be set by the build system (-DHL_QJS_VERSION=\"...\")"
#endif

#define QJS_TAG "qjs-" HL_QJS_VERSION

#endif /* HL_QUICKJS_TAG_H */
