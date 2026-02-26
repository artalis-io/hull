/*
 * cap/body.h — Body reader factory and extraction
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_BODY_H
#define HL_CAP_BODY_H

#include <stddef.h>
#include <keel/body_reader.h>

KlBodyReader *hl_cap_body_factory(KlAllocator *alloc, const KlRequest *req,
                                  void *user_data);

size_t hl_cap_body_data(const KlBodyReader *reader, const char **out_data);

#endif /* HL_CAP_BODY_H */
