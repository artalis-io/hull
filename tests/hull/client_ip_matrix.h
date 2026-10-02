/* client_ip_matrix.h - what hull.web._request.client_ip / clientIp returns
 * for the case matrix in test_lua.c and test_js.c (lua_stdlib / js_stdlib
 * .client_ip_matrix). One string, asserted in both runtimes, so the two
 * cannot drift.
 *
 * In order: remote_addr; remote_addr (XFF untrusted); last of the chain;
 * trimmed last; remote_addr fallback; remote_addr (empty XFF); nil (no
 * address); the 64-character cap; nil for a nil request.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_TEST_CLIENT_IP_MATRIX_H
#define HL_TEST_CLIENT_IP_MATRIX_H

#define HL_TEST_CLIENT_IP_MATRIX \
    "10.0.0.1|10.0.0.1|c|x|10.0.0.1|10.0.0.1|(nil)|" \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|(nil)"

#endif
