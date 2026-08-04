/*
 * runtime/factory_none.c - explicit "no composed runtime factory" default.
 *
 * The base no longer ships a WEAK hl_runtime_feature_factories() default (see
 * runtime/factory.c for why: a weak default in the always-linked collector TU
 * shadows a strong override that lives in a sibling archive member). Removing it
 * makes a produced app that forgets to emit its strong runtime-factory override
 * fail at LINK TIME ("undefined reference") instead of booting with no runtime.
 *
 * Non-app link targets that legitimately compose no runtime - the unit-test
 * binaries, which init runtimes directly rather than through the hook - link
 * THIS explicit strong empty default, opting in to "no composed factory" instead
 * of getting it by accident. It must NOT be linked alongside a real registry
 * (the hull toolchain registry or a produced app's emitted one): two strong defs
 * are a multiple-definition error, which is the intended, loud signal.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/factory.h"

#include <stddef.h>

const HlRuntimeFactory *const *hl_runtime_feature_factories(size_t *count)
{
    if (count) *count = 0;
    return NULL;
}
