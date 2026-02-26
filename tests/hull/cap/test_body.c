/*
 * test_hull_cap_body.c — Tests for body reader capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap.h"
#include <keel/body_reader.h>
#include <string.h>

UTEST(hl_cap_body, null_reader_returns_zero)
{
    const char *data = (const char *)0x1; /* sentinel */
    size_t len = hl_cap_body_data(NULL, &data);
    ASSERT_EQ((size_t)0, len);
    ASSERT_EQ(NULL, data);
}

UTEST(hl_cap_body, fake_buf_reader_extracts_data)
{
    /* Construct a fake KlBufReader with known data */
    KlBufReader br;
    memset(&br, 0, sizeof(br));
    br.data = (char *)"hello";
    br.len = 5;

    const char *data;
    size_t len = hl_cap_body_data((KlBodyReader *)&br, &data);
    ASSERT_EQ((size_t)5, len);
    ASSERT_EQ(0, memcmp(data, "hello", 5));
}

UTEST(hl_cap_body, empty_buf_reader_returns_zero)
{
    KlBufReader br;
    memset(&br, 0, sizeof(br));
    br.data = NULL;
    br.len = 0;

    const char *data;
    size_t len = hl_cap_body_data((KlBodyReader *)&br, &data);
    ASSERT_EQ((size_t)0, len);
}

UTEST_MAIN();
