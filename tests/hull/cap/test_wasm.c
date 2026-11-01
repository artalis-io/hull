/*
 * test_hull_cap_wasm.c - Tests for WASM compute capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm.h"
#include "hull/cap/wasm_aot_stamp.h"
#include "hull/cap/wasm_buffer.h"
#include "hull/cap/wasm_stream.h"
#include "hull/cap/wasm_watchdog.h"
#include "hull/utils/alloc.h"
#include "hull/limits/wasm.h"
#include "hull/shared/offheap.h"
#include "wasm_export.h"
#include <time.h>
#include "hull/shared/log_lock.h"
#include "log.h"
#include "hull/vfs.h"
#include "hull/entry.h"
#include "gen_echo64_aot.h" /* build-generated: echo64_aot[] + _len (0 if no wamrc) */
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

/* Pre-compiled echo.wasm (135 bytes) - copies input to output */
static const unsigned char echo_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x14, 0x03, 0x60,
  0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f,
  0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f, 0x02, 0x11, 0x01, 0x03, 0x65, 0x6e,
  0x76, 0x09, 0x68, 0x6f, 0x73, 0x74, 0x5f, 0x63, 0x61, 0x6c, 0x6c, 0x00,
  0x00, 0x03, 0x03, 0x02, 0x01, 0x02, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07,
  0x28, 0x03, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0c,
  0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73,
  0x00, 0x01, 0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x76, 0x65, 0x72, 0x73,
  0x69, 0x6f, 0x6e, 0x00, 0x02, 0x0a, 0x20, 0x02, 0x19, 0x00, 0x20, 0x01,
  0x20, 0x03, 0x4b, 0x04, 0x40, 0x41, 0x7e, 0x0f, 0x0b, 0x20, 0x02, 0x20,
  0x00, 0x20, 0x01, 0xfc, 0x0a, 0x00, 0x00, 0x20, 0x01, 0x0b, 0x04, 0x00,
  0x41, 0x01, 0x0b
};
static const unsigned int echo_wasm_len = 135;

/* Pre-compiled simd_dot_product_simd.wasm (906 bytes) - SIMD128 dot product.
 * Uses v128 types; requires AOT or SIMD-enabled interpreter to run. */
static const unsigned char simd_dot_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0d, 0x02, 0x60,
  0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f, 0x03,
  0x03, 0x02, 0x00, 0x01, 0x05, 0x05, 0x01, 0x01, 0x02, 0x80, 0x08, 0x06,
  0x08, 0x01, 0x7f, 0x01, 0x41, 0x80, 0x88, 0x04, 0x0b, 0x07, 0x28, 0x03,
  0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0c, 0x68, 0x75,
  0x6c, 0x6c, 0x5f, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73, 0x00, 0x00,
  0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f,
  0x6e, 0x00, 0x01, 0x0a, 0xf1, 0x04, 0x02, 0xe9, 0x04, 0x04, 0x02, 0x7f,
  0x01, 0x7b, 0x04, 0x7f, 0x01, 0x7c, 0x41, 0x7e, 0x21, 0x04, 0x02, 0x40,
  0x20, 0x01, 0x41, 0x04, 0x48, 0x0d, 0x00, 0x20, 0x03, 0x41, 0x08, 0x48,
  0x0d, 0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x22, 0x05, 0x41, 0x03, 0x74,
  0x41, 0x04, 0x72, 0x20, 0x01, 0x4a, 0x0d, 0x00, 0x20, 0x05, 0x41, 0x02,
  0x74, 0x21, 0x03, 0x02, 0x40, 0x02, 0x40, 0x20, 0x05, 0x41, 0x7c, 0x71,
  0x22, 0x01, 0x0d, 0x00, 0xfd, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x06,
  0x41, 0x00, 0x21, 0x01, 0x0c, 0x01, 0x0b, 0x20, 0x01, 0x41, 0x7f, 0x6a,
  0x22, 0x07, 0x41, 0x02, 0x76, 0x41, 0x01, 0x6a, 0x22, 0x04, 0x41, 0x03,
  0x71, 0x21, 0x08, 0x02, 0x40, 0x02, 0x40, 0x20, 0x01, 0x41, 0x0d, 0x4f,
  0x0d, 0x00, 0x41, 0x00, 0x21, 0x01, 0xfd, 0x0c, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x21, 0x06, 0x0c, 0x01, 0x0b, 0x20, 0x04, 0x41, 0xfc, 0xff, 0xff, 0xff,
  0x07, 0x71, 0x21, 0x09, 0x20, 0x01, 0x41, 0x73, 0x6a, 0x41, 0x70, 0x71,
  0x21, 0x0a, 0xfd, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x06, 0x20, 0x00,
  0x21, 0x01, 0x03, 0x40, 0x20, 0x06, 0x20, 0x01, 0x41, 0x04, 0x6a, 0xfd,
  0x00, 0x00, 0x00, 0x20, 0x01, 0x20, 0x03, 0x6a, 0x22, 0x04, 0x41, 0x04,
  0x6a, 0xfd, 0x00, 0x00, 0x00, 0xfd, 0xe6, 0x01, 0xfd, 0xe4, 0x01, 0x20,
  0x01, 0x41, 0x14, 0x6a, 0xfd, 0x00, 0x00, 0x00, 0x20, 0x04, 0x41, 0x14,
  0x6a, 0xfd, 0x00, 0x00, 0x00, 0xfd, 0xe6, 0x01, 0xfd, 0xe4, 0x01, 0x20,
  0x01, 0x41, 0x24, 0x6a, 0xfd, 0x00, 0x00, 0x00, 0x20, 0x04, 0x41, 0x24,
  0x6a, 0xfd, 0x00, 0x00, 0x00, 0xfd, 0xe6, 0x01, 0xfd, 0xe4, 0x01, 0x20,
  0x01, 0x41, 0x34, 0x6a, 0xfd, 0x00, 0x00, 0x00, 0x20, 0x04, 0x41, 0x34,
  0x6a, 0xfd, 0x00, 0x00, 0x00, 0xfd, 0xe6, 0x01, 0xfd, 0xe4, 0x01, 0x21,
  0x06, 0x20, 0x01, 0x41, 0xc0, 0x00, 0x6a, 0x21, 0x01, 0x20, 0x09, 0x41,
  0x7c, 0x6a, 0x22, 0x09, 0x0d, 0x00, 0x0b, 0x20, 0x0a, 0x41, 0x10, 0x6a,
  0x21, 0x01, 0x0b, 0x20, 0x07, 0x41, 0x7c, 0x71, 0x21, 0x04, 0x02, 0x40,
  0x20, 0x08, 0x45, 0x0d, 0x00, 0x20, 0x01, 0x41, 0x02, 0x74, 0x20, 0x00,
  0x6a, 0x41, 0x04, 0x6a, 0x21, 0x01, 0x03, 0x40, 0x20, 0x06, 0x20, 0x01,
  0xfd, 0x00, 0x00, 0x00, 0x20, 0x01, 0x20, 0x03, 0x6a, 0xfd, 0x00, 0x00,
  0x00, 0xfd, 0xe6, 0x01, 0xfd, 0xe4, 0x01, 0x21, 0x06, 0x20, 0x01, 0x41,
  0x10, 0x6a, 0x21, 0x01, 0x20, 0x08, 0x41, 0x7f, 0x6a, 0x22, 0x08, 0x0d,
  0x00, 0x0b, 0x0b, 0x20, 0x04, 0x41, 0x04, 0x6a, 0x21, 0x01, 0x0b, 0x20,
  0x06, 0xfd, 0x1f, 0x00, 0xbb, 0x20, 0x06, 0xfd, 0x1f, 0x01, 0xbb, 0xa0,
  0x20, 0x06, 0xfd, 0x1f, 0x02, 0xbb, 0xa0, 0x20, 0x06, 0xfd, 0x1f, 0x03,
  0xbb, 0xa0, 0x21, 0x0b, 0x02, 0x40, 0x20, 0x05, 0x20, 0x01, 0x4d, 0x0d,
  0x00, 0x20, 0x01, 0x41, 0x01, 0x6a, 0x21, 0x04, 0x02, 0x40, 0x20, 0x05,
  0x20, 0x01, 0x6b, 0x41, 0x01, 0x71, 0x45, 0x0d, 0x00, 0x20, 0x00, 0x41,
  0x04, 0x6a, 0x22, 0x08, 0x20, 0x01, 0x41, 0x02, 0x74, 0x22, 0x01, 0x6a,
  0x2a, 0x02, 0x00, 0xbb, 0x20, 0x08, 0x20, 0x03, 0x6a, 0x20, 0x01, 0x6a,
  0x2a, 0x02, 0x00, 0xbb, 0xa2, 0x20, 0x0b, 0xa0, 0x21, 0x0b, 0x20, 0x04,
  0x21, 0x01, 0x0b, 0x20, 0x05, 0x20, 0x04, 0x46, 0x0d, 0x00, 0x20, 0x05,
  0x20, 0x01, 0x6b, 0x21, 0x04, 0x20, 0x00, 0x20, 0x01, 0x41, 0x02, 0x74,
  0x6a, 0x21, 0x01, 0x03, 0x40, 0x20, 0x01, 0x41, 0x08, 0x6a, 0x22, 0x08,
  0x2a, 0x02, 0x00, 0xbb, 0x20, 0x01, 0x20, 0x03, 0x6a, 0x22, 0x09, 0x41,
  0x08, 0x6a, 0x2a, 0x02, 0x00, 0xbb, 0xa2, 0x20, 0x01, 0x41, 0x04, 0x6a,
  0x2a, 0x02, 0x00, 0xbb, 0x20, 0x09, 0x41, 0x04, 0x6a, 0x2a, 0x02, 0x00,
  0xbb, 0xa2, 0x20, 0x0b, 0xa0, 0xa0, 0x21, 0x0b, 0x20, 0x08, 0x21, 0x01,
  0x20, 0x04, 0x41, 0x7e, 0x6a, 0x22, 0x04, 0x0d, 0x00, 0x0b, 0x0b, 0x20,
  0x02, 0x20, 0x0b, 0x39, 0x03, 0x00, 0x41, 0x08, 0x21, 0x04, 0x0b, 0x20,
  0x04, 0x0b, 0x04, 0x00, 0x41, 0x01, 0x0b, 0x00, 0x55, 0x04, 0x6e, 0x61,
  0x6d, 0x65, 0x00, 0x1b, 0x1a, 0x73, 0x69, 0x6d, 0x64, 0x5f, 0x64, 0x6f,
  0x74, 0x5f, 0x70, 0x72, 0x6f, 0x64, 0x75, 0x63, 0x74, 0x5f, 0x73, 0x69,
  0x6d, 0x64, 0x2e, 0x77, 0x61, 0x73, 0x6d, 0x01, 0x1d, 0x02, 0x00, 0x0c,
  0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73,
  0x01, 0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x76, 0x65, 0x72, 0x73, 0x69,
  0x6f, 0x6e, 0x07, 0x12, 0x01, 0x00, 0x0f, 0x5f, 0x5f, 0x73, 0x74, 0x61,
  0x63, 0x6b, 0x5f, 0x70, 0x6f, 0x69, 0x6e, 0x74, 0x65, 0x72, 0x00, 0x2f,
  0x09, 0x70, 0x72, 0x6f, 0x64, 0x75, 0x63, 0x65, 0x72, 0x73, 0x01, 0x0c,
  0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73, 0x65, 0x64, 0x2d, 0x62, 0x79,
  0x01, 0x0e, 0x48, 0x6f, 0x6d, 0x65, 0x62, 0x72, 0x65, 0x77, 0x20, 0x63,
  0x6c, 0x61, 0x6e, 0x67, 0x06, 0x31, 0x38, 0x2e, 0x31, 0x2e, 0x38, 0x00,
  0x35, 0x0f, 0x74, 0x61, 0x72, 0x67, 0x65, 0x74, 0x5f, 0x66, 0x65, 0x61,
  0x74, 0x75, 0x72, 0x65, 0x73, 0x03, 0x2b, 0x0f, 0x6d, 0x75, 0x74, 0x61,
  0x62, 0x6c, 0x65, 0x2d, 0x67, 0x6c, 0x6f, 0x62, 0x61, 0x6c, 0x73, 0x2b,
  0x08, 0x73, 0x69, 0x67, 0x6e, 0x2d, 0x65, 0x78, 0x74, 0x2b, 0x07, 0x73,
  0x69, 0x6d, 0x64, 0x31, 0x32, 0x38
};
static const unsigned int simd_dot_wasm_len = 906;

/* Pre-compiled kv_store.wasm (674 bytes) - stateful key-value store.
 * Opcodes: 0x01=LOAD, 0x02=GET, 0x03=COUNT.
 * State persists in WASM globals across calls on persistent instances. */
static const unsigned char kv_store_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x14, 0x03, 0x60,
  0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f,
  0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f, 0x02, 0x11, 0x01, 0x03, 0x65, 0x6e,
  0x76, 0x09, 0x68, 0x6f, 0x73, 0x74, 0x5f, 0x63, 0x61, 0x6c, 0x6c, 0x00,
  0x00, 0x03, 0x03, 0x02, 0x01, 0x02, 0x05, 0x03, 0x01, 0x00, 0x10, 0x06,
  0x14, 0x03, 0x7f, 0x01, 0x41, 0x80, 0x80, 0x04, 0x0b, 0x7f, 0x01, 0x41,
  0x00, 0x0b, 0x7f, 0x01, 0x41, 0x80, 0x80, 0x04, 0x0b, 0x07, 0x28, 0x03,
  0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0c, 0x68, 0x75,
  0x6c, 0x6c, 0x5f, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73, 0x00, 0x01,
  0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f,
  0x6e, 0x00, 0x02, 0x0a, 0xa4, 0x04, 0x02, 0x9c, 0x04, 0x01, 0x0e, 0x7f,
  0x20, 0x01, 0x41, 0x01, 0x49, 0x04, 0x40, 0x41, 0x7f, 0x0f, 0x0b, 0x20,
  0x00, 0x2d, 0x00, 0x00, 0x21, 0x04, 0x20, 0x04, 0x41, 0x01, 0x46, 0x04,
  0x40, 0x20, 0x01, 0x41, 0x05, 0x49, 0x04, 0x40, 0x41, 0x7f, 0x0f, 0x0b,
  0x20, 0x00, 0x41, 0x01, 0x6a, 0x28, 0x02, 0x00, 0x21, 0x06, 0x41, 0x00,
  0x24, 0x01, 0x23, 0x00, 0x24, 0x02, 0x23, 0x00, 0x21, 0x09, 0x41, 0x05,
  0x21, 0x05, 0x41, 0x00, 0x21, 0x0a, 0x02, 0x40, 0x03, 0x40, 0x20, 0x0a,
  0x20, 0x06, 0x4f, 0x0d, 0x01, 0x20, 0x05, 0x41, 0x04, 0x6a, 0x20, 0x01,
  0x4b, 0x04, 0x40, 0x0c, 0x02, 0x0b, 0x20, 0x00, 0x20, 0x05, 0x6a, 0x28,
  0x02, 0x00, 0x21, 0x07, 0x20, 0x05, 0x41, 0x04, 0x6a, 0x21, 0x05, 0x20,
  0x09, 0x20, 0x07, 0x36, 0x02, 0x00, 0x20, 0x09, 0x41, 0x04, 0x6a, 0x21,
  0x09, 0x20, 0x07, 0x41, 0x00, 0x4b, 0x04, 0x40, 0x20, 0x09, 0x20, 0x00,
  0x20, 0x05, 0x6a, 0x20, 0x07, 0xfc, 0x0a, 0x00, 0x00, 0x0b, 0x20, 0x09,
  0x20, 0x07, 0x6a, 0x21, 0x09, 0x20, 0x05, 0x20, 0x07, 0x6a, 0x21, 0x05,
  0x20, 0x05, 0x41, 0x04, 0x6a, 0x20, 0x01, 0x4b, 0x04, 0x40, 0x0c, 0x02,
  0x0b, 0x20, 0x00, 0x20, 0x05, 0x6a, 0x28, 0x02, 0x00, 0x21, 0x08, 0x20,
  0x05, 0x41, 0x04, 0x6a, 0x21, 0x05, 0x20, 0x09, 0x20, 0x08, 0x36, 0x02,
  0x00, 0x20, 0x09, 0x41, 0x04, 0x6a, 0x21, 0x09, 0x20, 0x08, 0x41, 0x00,
  0x4b, 0x04, 0x40, 0x20, 0x09, 0x20, 0x00, 0x20, 0x05, 0x6a, 0x20, 0x08,
  0xfc, 0x0a, 0x00, 0x00, 0x0b, 0x20, 0x09, 0x20, 0x08, 0x6a, 0x21, 0x09,
  0x20, 0x05, 0x20, 0x08, 0x6a, 0x21, 0x05, 0x23, 0x01, 0x41, 0x01, 0x6a,
  0x24, 0x01, 0x20, 0x0a, 0x41, 0x01, 0x6a, 0x21, 0x0a, 0x0c, 0x00, 0x0b,
  0x0b, 0x20, 0x09, 0x24, 0x02, 0x20, 0x03, 0x41, 0x04, 0x49, 0x04, 0x40,
  0x41, 0x7e, 0x0f, 0x0b, 0x20, 0x02, 0x23, 0x01, 0x36, 0x02, 0x00, 0x41,
  0x04, 0x0f, 0x0b, 0x20, 0x04, 0x41, 0x02, 0x46, 0x04, 0x40, 0x20, 0x00,
  0x41, 0x01, 0x6a, 0x21, 0x10, 0x20, 0x01, 0x41, 0x01, 0x6b, 0x21, 0x11,
  0x23, 0x00, 0x21, 0x0b, 0x41, 0x00, 0x21, 0x0a, 0x02, 0x40, 0x03, 0x40,
  0x20, 0x0a, 0x23, 0x01, 0x4f, 0x0d, 0x01, 0x20, 0x0b, 0x23, 0x02, 0x4f,
  0x0d, 0x01, 0x20, 0x0b, 0x28, 0x02, 0x00, 0x21, 0x0c, 0x20, 0x0b, 0x41,
  0x04, 0x6a, 0x21, 0x0b, 0x20, 0x0c, 0x20, 0x11, 0x46, 0x04, 0x40, 0x41,
  0x01, 0x21, 0x0e, 0x41, 0x00, 0x21, 0x0f, 0x02, 0x40, 0x03, 0x40, 0x20,
  0x0f, 0x20, 0x0c, 0x4f, 0x0d, 0x01, 0x20, 0x0b, 0x20, 0x0f, 0x6a, 0x2d,
  0x00, 0x00, 0x20, 0x10, 0x20, 0x0f, 0x6a, 0x2d, 0x00, 0x00, 0x47, 0x04,
  0x40, 0x41, 0x00, 0x21, 0x0e, 0x0c, 0x02, 0x0b, 0x20, 0x0f, 0x41, 0x01,
  0x6a, 0x21, 0x0f, 0x0c, 0x00, 0x0b, 0x0b, 0x20, 0x0e, 0x04, 0x40, 0x20,
  0x0b, 0x20, 0x0c, 0x6a, 0x21, 0x0b, 0x20, 0x0b, 0x28, 0x02, 0x00, 0x21,
  0x0d, 0x20, 0x0b, 0x41, 0x04, 0x6a, 0x21, 0x0b, 0x20, 0x0d, 0x20, 0x03,
  0x4b, 0x04, 0x40, 0x41, 0x7e, 0x0f, 0x0b, 0x20, 0x0d, 0x41, 0x00, 0x4b,
  0x04, 0x40, 0x20, 0x02, 0x20, 0x0b, 0x20, 0x0d, 0xfc, 0x0a, 0x00, 0x00,
  0x0b, 0x20, 0x0d, 0x0f, 0x0b, 0x0b, 0x20, 0x0b, 0x20, 0x0c, 0x6a, 0x21,
  0x0b, 0x20, 0x0b, 0x28, 0x02, 0x00, 0x21, 0x0d, 0x20, 0x0b, 0x41, 0x04,
  0x6a, 0x21, 0x0b, 0x20, 0x0b, 0x20, 0x0d, 0x6a, 0x21, 0x0b, 0x20, 0x0a,
  0x41, 0x01, 0x6a, 0x21, 0x0a, 0x0c, 0x00, 0x0b, 0x0b, 0x41, 0x00, 0x0f,
  0x0b, 0x20, 0x04, 0x41, 0x03, 0x46, 0x04, 0x40, 0x20, 0x03, 0x41, 0x04,
  0x49, 0x04, 0x40, 0x41, 0x7e, 0x0f, 0x0b, 0x20, 0x02, 0x23, 0x01, 0x36,
  0x02, 0x00, 0x41, 0x04, 0x0f, 0x0b, 0x41, 0x7f, 0x0b, 0x04, 0x00, 0x41,
  0x01, 0x0b
};
static const unsigned int kv_store_wasm_len = 674;

/* Pre-compiled shared_read.wasm (306 bytes) - reads from shared heap via host_call */
static const unsigned char shared_read_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x14, 0x03, 0x60,
  0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f,
  0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f, 0x02, 0x11, 0x01, 0x03, 0x65, 0x6e,
  0x76, 0x09, 0x68, 0x6f, 0x73, 0x74, 0x5f, 0x63, 0x61, 0x6c, 0x6c, 0x00,
  0x00, 0x03, 0x03, 0x02, 0x01, 0x02, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07,
  0x28, 0x03, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0c,
  0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73,
  0x00, 0x01, 0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x76, 0x65, 0x72, 0x73,
  0x69, 0x6f, 0x6e, 0x00, 0x02, 0x0a, 0xca, 0x01, 0x02, 0xc2, 0x01, 0x01,
  0x07, 0x7f, 0x20, 0x01, 0x41, 0x01, 0x49, 0x04, 0x40, 0x41, 0x7f, 0x0f,
  0x0b, 0x20, 0x00, 0x2d, 0x00, 0x00, 0x21, 0x04, 0x20, 0x04, 0x41, 0xff,
  0x01, 0x46, 0x04, 0x40, 0x41, 0x02, 0x41, 0x7f, 0x41, 0x00, 0x10, 0x00,
  0x21, 0x07, 0x20, 0x03, 0x41, 0x04, 0x49, 0x04, 0x40, 0x41, 0x7e, 0x0f,
  0x0b, 0x20, 0x02, 0x20, 0x07, 0x36, 0x02, 0x00, 0x41, 0x04, 0x0f, 0x0b,
  0x20, 0x01, 0x41, 0x09, 0x49, 0x04, 0x40, 0x41, 0x7f, 0x0f, 0x0b, 0x20,
  0x00, 0x41, 0x01, 0x6a, 0x28, 0x02, 0x00, 0x21, 0x05, 0x20, 0x00, 0x41,
  0x05, 0x6a, 0x28, 0x02, 0x00, 0x21, 0x06, 0x41, 0x02, 0x20, 0x04, 0x41,
  0x00, 0x10, 0x00, 0x21, 0x08, 0x20, 0x08, 0x45, 0x04, 0x40, 0x41, 0x00,
  0x0f, 0x0b, 0x41, 0x02, 0x20, 0x04, 0x41, 0x01, 0x10, 0x00, 0x21, 0x09,
  0x20, 0x05, 0x20, 0x06, 0x6a, 0x20, 0x09, 0x4b, 0x04, 0x40, 0x41, 0x7f,
  0x0f, 0x0b, 0x20, 0x06, 0x20, 0x03, 0x4b, 0x04, 0x40, 0x41, 0x7e, 0x0f,
  0x0b, 0x41, 0x00, 0x21, 0x0a, 0x02, 0x40, 0x03, 0x40, 0x20, 0x0a, 0x20,
  0x06, 0x4f, 0x0d, 0x01, 0x20, 0x02, 0x20, 0x0a, 0x6a, 0x20, 0x08, 0x20,
  0x05, 0x6a, 0x20, 0x0a, 0x6a, 0x2d, 0x00, 0x00, 0x3a, 0x00, 0x00, 0x20,
  0x0a, 0x41, 0x01, 0x6a, 0x21, 0x0a, 0x0c, 0x00, 0x0b, 0x0b, 0x20, 0x06,
  0x0b, 0x04, 0x00, 0x41, 0x01, 0x0b
};
static const unsigned int shared_read_wasm_len = 306;

/* Pre-compiled echo64.wasm (136 bytes) - Memory64 echo module */
static const unsigned char echo64_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x14, 0x03, 0x60,
  0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x04, 0x7e, 0x7e, 0x7e, 0x7e,
  0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f, 0x02, 0x11, 0x01, 0x03, 0x65, 0x6e,
  0x76, 0x09, 0x68, 0x6f, 0x73, 0x74, 0x5f, 0x63, 0x61, 0x6c, 0x6c, 0x00,
  0x00, 0x03, 0x03, 0x02, 0x01, 0x02, 0x05, 0x03, 0x01, 0x04, 0x01, 0x07,
  0x28, 0x03, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0c,
  0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73,
  0x00, 0x01, 0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x76, 0x65, 0x72, 0x73,
  0x69, 0x6f, 0x6e, 0x00, 0x02, 0x0a, 0x21, 0x02, 0x1a, 0x00, 0x20, 0x01,
  0x20, 0x03, 0x56, 0x04, 0x40, 0x41, 0x7e, 0x0f, 0x0b, 0x20, 0x02, 0x20,
  0x00, 0x20, 0x01, 0xfc, 0x0a, 0x00, 0x00, 0x20, 0x01, 0xa7, 0x0b, 0x04,
  0x00, 0x41, 0x01, 0x0b
};
static const unsigned int echo64_wasm_len = 136;

/* loop_forever.wasm (67 bytes): hull_process = (loop br 0) - never returns.
 * Hand-assembled; see the watchdog tests below. */
static const unsigned char loop_forever_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x09, 0x01, 0x60,
  0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x05,
  0x03, 0x01, 0x00, 0x01, 0x07, 0x19, 0x02, 0x06, 0x6d, 0x65, 0x6d, 0x6f,
  0x72, 0x79, 0x02, 0x00, 0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x70, 0x72,
  0x6f, 0x63, 0x65, 0x73, 0x73, 0x00, 0x00, 0x0a, 0x0a, 0x01, 0x08, 0x00,
  0x03, 0x40, 0x0c, 0x00, 0x0b, 0x00, 0x0b,
};
static const unsigned int loop_forever_wasm_len = 67;

/* start_loop.wasm (78 bytes): a start function that never returns;
 * hull_process returns 0. */
static const unsigned char start_loop_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0c, 0x02, 0x60,
  0x00, 0x00, 0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x03,
  0x02, 0x00, 0x01, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x19, 0x02, 0x06,
  0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0c, 0x68, 0x75, 0x6c,
  0x6c, 0x5f, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73, 0x00, 0x01, 0x08,
  0x01, 0x00, 0x0a, 0x0e, 0x02, 0x07, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b,
  0x0b, 0x04, 0x00, 0x41, 0x00, 0x0b,
};
static const unsigned int start_loop_wasm_len = 78;

/* grow.wasm (68 bytes): one memory (1 page, no maximum); hull_process returns
 * memory.grow(input_len) != -1 - i.e. it grows linear memory by one page per
 * input byte and returns 1 (one output byte) when that worked, 0 when not.
 * Hand-assembled; see the memory.grow tests below. */
static const unsigned char grow_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x09, 0x01, 0x60,
  0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x05,
  0x03, 0x01, 0x00, 0x01, 0x07, 0x19, 0x02, 0x06, 0x6d, 0x65, 0x6d, 0x6f,
  0x72, 0x79, 0x02, 0x00, 0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x70, 0x72,
  0x6f, 0x63, 0x65, 0x73, 0x73, 0x00, 0x00, 0x0a, 0x0b, 0x01, 0x09, 0x00,
  0x20, 0x01, 0x40, 0x00, 0x41, 0x7f, 0x47, 0x0b,
};
static const unsigned int grow_wasm_len = 68;

/* overrun.wasm: hull_process(in, in_len, out, out_cap) returns out_cap + 1 -
 * a guest that reports more output than the buffer it was given holds.
 * grow_wasm with the body `local.get 3; i32.const 1; i32.add`. */
static const unsigned char overrun_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x09, 0x01, 0x60,
  0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x05,
  0x03, 0x01, 0x00, 0x01, 0x07, 0x19, 0x02, 0x06, 0x6d, 0x65, 0x6d, 0x6f,
  0x72, 0x79, 0x02, 0x00, 0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x70, 0x72,
  0x6f, 0x63, 0x65, 0x73, 0x73, 0x00, 0x00, 0x0a, 0x09, 0x01, 0x07, 0x00,
  0x20, 0x03, 0x41, 0x01, 0x6a, 0x0b,
};
static const unsigned int overrun_wasm_len = 66;

/* guest_alloc.wasm (102 bytes): exports memory, malloc, free and
 * hull_process. Its malloc returns 0x7fffff00 - far outside its one page -
 * which WAMR would hand back as memory_data + offset, unchecked, for every
 * wasm_runtime_module_malloc Hull makes. Built with wat2wasm from:
 *   (module (memory (export "memory") 1)
 *     (func (export "malloc") (param i32) (result i32) i32.const 0x7fffff00)
 *     (func (export "free") (param i32))
 *     (func (export "hull_process") (param i32 i32 i32 i32) (result i32)
 *       i32.const 0)) */
static const unsigned char guest_alloc_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x12, 0x03, 0x60,
  0x01, 0x7f, 0x01, 0x7f, 0x60, 0x01, 0x7f, 0x00, 0x60, 0x04, 0x7f, 0x7f,
  0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x04, 0x03, 0x00, 0x01, 0x02, 0x05, 0x03,
  0x01, 0x00, 0x01, 0x07, 0x29, 0x04, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72,
  0x79, 0x02, 0x00, 0x06, 0x6d, 0x61, 0x6c, 0x6c, 0x6f, 0x63, 0x00, 0x00,
  0x04, 0x66, 0x72, 0x65, 0x65, 0x00, 0x01, 0x0c, 0x68, 0x75, 0x6c, 0x6c,
  0x5f, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73, 0x00, 0x02, 0x0a, 0x12,
  0x03, 0x08, 0x00, 0x41, 0x80, 0xfe, 0xff, 0xff, 0x07, 0x0b, 0x02, 0x00,
  0x0b, 0x04, 0x00, 0x41, 0x00, 0x0b
};
static const unsigned int guest_alloc_wasm_len = 102;

/* Audit 41. big_out.wasm: hull_process returns its out_cap - a guest that
 * hands back max_output bytes for any input. Built with wat2wasm from:
 *   (module (memory (export "memory") 2)
 *     (func (export "hull_process") (param i32 i32 i32 i32) (result i32)
 *       local.get 3)) */
static const unsigned char big_out_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x09, 0x01, 0x60,
  0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x05,
  0x03, 0x01, 0x00, 0x02, 0x07, 0x19, 0x02, 0x06, 0x6d, 0x65, 0x6d, 0x6f,
  0x72, 0x79, 0x02, 0x00, 0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x70, 0x72,
  0x6f, 0x63, 0x65, 0x73, 0x73, 0x00, 0x00, 0x0a, 0x06, 0x01, 0x04, 0x00,
  0x20, 0x03, 0x0b
};
static const unsigned int big_out_wasm_len = 63;

/* Audit 41. logspam.wasm: hull_process logs a 100-byte line 1000 times
 * (host_call(HL_WASM_OP_LOG, 1024, 100)) and returns 0. wat2wasm from:
 *   (module (import "env" "host_call" (func $hc (param i32 i32 i32) (result i32)))
 *     (memory (export "memory") 1)
 *     (func (export "hull_process") (param i32 i32 i32 i32) (result i32)
 *       (local $i i32)
 *       (memory.fill (i32.const 1024) (i32.const 65) (i32.const 100))
 *       (block $done (loop $l
 *         (br_if $done (i32.ge_u (local.get $i) (i32.const 1000)))
 *         (drop (call $hc (i32.const 1) (i32.const 1024) (i32.const 100)))
 *         (local.set $i (i32.add (local.get $i) (i32.const 1)))
 *         (br $l)))
 *       i32.const 0)) */
static const unsigned char logspam_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x02, 0x60,
  0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f,
  0x01, 0x7f, 0x02, 0x11, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x09, 0x68, 0x6f,
  0x73, 0x74, 0x5f, 0x63, 0x61, 0x6c, 0x6c, 0x00, 0x00, 0x03, 0x02, 0x01,
  0x01, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x19, 0x02, 0x06, 0x6d, 0x65,
  0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f,
  0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73, 0x00, 0x01, 0x0a, 0x36, 0x01,
  0x34, 0x01, 0x01, 0x7f, 0x41, 0x80, 0x08, 0x41, 0xc1, 0x00, 0x41, 0xe4,
  0x00, 0xfc, 0x0b, 0x00, 0x02, 0x40, 0x03, 0x40, 0x20, 0x04, 0x41, 0xe8,
  0x07, 0x4f, 0x0d, 0x01, 0x41, 0x01, 0x41, 0x80, 0x08, 0x41, 0xe4, 0x00,
  0x10, 0x00, 0x1a, 0x20, 0x04, 0x41, 0x01, 0x6a, 0x21, 0x04, 0x0c, 0x00,
  0x0b, 0x0b, 0x41, 0x00, 0x0b
};
static const unsigned int logspam_wasm_len = 137;

/* VFS with embedded WASM modules for testing (sorted by name) */
static const HlEntry test_entries[] = {
    { "compute/big_out.wasm", big_out_wasm, big_out_wasm_len },
    { "compute/echo.wasm", echo_wasm, echo_wasm_len },
    { "compute/echo64.wasm", echo64_wasm, echo64_wasm_len },
    { "compute/grow.wasm", grow_wasm, grow_wasm_len },
    { "compute/guest_alloc.wasm", guest_alloc_wasm, guest_alloc_wasm_len },
    { "compute/kv_store.wasm", kv_store_wasm, kv_store_wasm_len },
    { "compute/logspam.wasm", logspam_wasm, logspam_wasm_len },
    { "compute/loop_forever.wasm", loop_forever_wasm, loop_forever_wasm_len },
    { "compute/overrun.wasm", overrun_wasm, overrun_wasm_len },
    { "compute/shared_read.wasm", shared_read_wasm, shared_read_wasm_len },
    { "compute/simd_dot.wasm", simd_dot_wasm, simd_dot_wasm_len },
    { "compute/start_loop.wasm", start_loop_wasm, start_loop_wasm_len },
    { 0, 0, 0 }
};

/* Audit 26: a guest's LOG message went to the log with CR / LF raw. */
UTEST(hl_cap_wasm, log_escape_controls)
{
    char out[64];
    const char msg[] = "a\nb\rc\x01" "d\x7f" "e";
    size_t n = hl_cap_wasm_log_escape(msg, sizeof msg - 1, out, sizeof out);
    ASSERT_STREQ("a\\nb\\rc\\x01d\\x7fe", out);
    ASSERT_EQ(strlen(out), n);
    /* An embedded NUL no longer cuts the message short. */
    const char nul[] = { 'x', '\0', 'y' };
    hl_cap_wasm_log_escape(nul, 3, out, sizeof out);
    ASSERT_STREQ("x\\x00y", out);
    /* UTF-8 passes through. */
    hl_cap_wasm_log_escape("\xc3\xa9", 2, out, sizeof out);
    ASSERT_STREQ("\xc3\xa9", out);
}

/* Audit 42: a guest's C1 controls (C2 80-9F) and stray 0x80-0x9F bytes went
 * to the log raw; each byte is \xNN now, other UTF-8 kept whole. */
UTEST(wasm_audit42, log_escape_c1_controls)
{
    char out[64];
    hl_cap_wasm_log_escape("a\xc2\x9b" "2J\x9d" "b", 7, out, sizeof out);
    ASSERT_STREQ("a\\xc2\\x9b2J\\x9db", out);
    hl_cap_wasm_log_escape("\xe2\x82\xac\xc2\xa0", 5, out, sizeof out);
    ASSERT_STREQ("\xe2\x82\xac\xc2\xa0", out);
    /* An escaped unit is never split by the "...": "a" + "..." fits 8. */
    hl_cap_wasm_log_escape("a\xc2\x9b" "bcdef", 8, out, 8);
    ASSERT_STREQ("a...", out);
    /* A kept multi-byte character is not cut in half either. */
    hl_cap_wasm_log_escape("ab\xf0\x9f\x98\x80" "cdef", 10, out, 8);
    ASSERT_STREQ("ab...", out);
}

UTEST(hl_cap_wasm, log_escape_truncates)
{
    char out[8];
    hl_cap_wasm_log_escape("\n\n\n\n\n\n", 6, out, sizeof out);
    ASSERT_TRUE(strlen(out) < sizeof out);
    ASSERT_TRUE(strstr(out, "...") != NULL);
    ASSERT_TRUE(strchr(out, '\n') == NULL);
    /* The "..." never splits an escape (fuzz_wasm_log_escape: "a\x0..."). */
    ASSERT_EQ((size_t)4, hl_cap_wasm_log_escape("a\x01" "bcdef", 7, out, sizeof out));
    ASSERT_STREQ("a...", out);
}

UTEST(hl_cap_wasm, init_destroy)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    ASSERT_EQ(cache.initialized, 1);
    ASSERT_EQ(cache.count, 0);
    hl_cap_wasm_destroy(&cache);
    ASSERT_EQ(cache.initialized, 0);
}

UTEST(hl_cap_wasm, load_from_vfs)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    int rc = hl_cap_wasm_load(&cache, "echo", &vfs, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(cache.count, 1);

    /* Second load should be a no-op (cached) */
    rc = hl_cap_wasm_load(&cache, "echo", &vfs, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(cache.count, 1);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, call_echo)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *input = "hello, wasm!";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, input_len,
                               &output, &output_len,
                               NULL, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_NE(output, NULL);
    ASSERT_EQ(memcmp(output, input, input_len), 0);

    free(output);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, call_empty_input)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               "", 0,
                               &output, &output_len,
                               NULL, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, (size_t)0);

    free(output);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, module_not_found)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "nonexistent",
                               "x", 1,
                               &output, &output_len,
                               NULL, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "not_found");
    ASSERT_EQ(output, NULL);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, input_size_limit)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* Set max_input to 4 bytes, then try to send 10 */
    HlWasmCallOpts opts = {0};
    opts.max_input = 4;

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               "0123456789", 10,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_INPUT);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "input_too_large");

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, gas_exhaustion)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* Set gas to 1 instruction - should fail */
    HlWasmCallOpts opts = {0};
    opts.gas = 1;

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               "hello", 5,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    /* Gas exhaustion must classify as HL_WASM_ERR_GAS + "gas_exhausted", not a
     * generic call failure. Regression for #314: WAMR 2.4.1 emits "instruction
     * limit exceeded" (older builds said "instruction count"); the classifier
     * must match either. */
    ASSERT_EQ(rc, HL_WASM_ERR_GAS);
    ASSERT_TRUE(err != NULL);
    ASSERT_STREQ(err, "gas_exhausted");

    free(output);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, abi_version)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);
    ASSERT_EQ(cache.modules[0].abi_version, (uint32_t)1);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, null_safety)
{
    ASSERT_EQ(hl_cap_wasm_init(NULL), -1);

    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    ASSERT_EQ(hl_cap_wasm_load(&cache, NULL, NULL, NULL), HL_WASM_ERR_INTERNAL);

    const char *err = NULL;
    ASSERT_EQ(hl_cap_wasm_call(NULL, "x", "y", 1, NULL, NULL,
                                NULL, NULL, NULL, NULL, NULL, NULL, &err),
              HL_WASM_ERR_INTERNAL);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, path_traversal_rejected)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    /* Slash in name - path traversal */
    int rc = hl_cap_wasm_call(&cache, "../../../etc/passwd",
                               "x", 1, &output, &output_len,
                               NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);
    ASSERT_EQ(output, NULL);

    /* Backslash in name */
    err = NULL;
    rc = hl_cap_wasm_call(&cache, "..\\secret",
                           "x", 1, &output, &output_len,
                           NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);

    /* Dot-prefixed name */
    err = NULL;
    rc = hl_cap_wasm_call(&cache, ".hidden",
                           "x", 1, &output, &output_len,
                           NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);

    /* Simple slash */
    err = NULL;
    rc = hl_cap_wasm_call(&cache, "sub/module",
                           "x", 1, &output, &output_len,
                           NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);

    /* Also check load path */
    ASSERT_EQ(hl_cap_wasm_load(&cache, "../escape", &vfs, NULL),
              HL_WASM_ERR_NOT_FOUND);
    ASSERT_EQ(hl_cap_wasm_load(&cache, ".dotfile", &vfs, NULL),
              HL_WASM_ERR_NOT_FOUND);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, overlong_name_rejected)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* 256-char name exceeds the 255 limit */
    char long_name[257];
    memset(long_name, 'a', 256);
    long_name[256] = '\0';

    ASSERT_EQ(hl_cap_wasm_load(&cache, long_name, &vfs, NULL),
              HL_WASM_ERR_NOT_FOUND);

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, long_name,
                               "x", 1, &output, &output_len,
                               NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);
    ASSERT_EQ(output, NULL);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, empty_name_rejected)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    ASSERT_EQ(hl_cap_wasm_load(&cache, "", NULL, NULL),
              HL_WASM_ERR_NOT_FOUND);

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "",
                               "x", 1, &output, &output_len,
                               NULL, NULL, NULL, NULL, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, destroy_idempotent)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);

    hl_cap_wasm_destroy(&cache);
    ASSERT_EQ(cache.initialized, 0);
    ASSERT_EQ(cache.count, 0);

    /* Second destroy should be a safe no-op */
    hl_cap_wasm_destroy(&cache);
    ASSERT_EQ(cache.initialized, 0);

    /* Destroy NULL should also be safe */
    hl_cap_wasm_destroy(NULL);
}

UTEST(hl_cap_wasm, call_with_custom_opts)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts opts = {0};
    opts.max_input  = 1024;
    opts.max_output = 1024;
    opts.heap_size  = 512 * 1024;
    opts.stack_size = 32 * 1024;
    opts.gas        = 50000000;

    const char *input = "custom opts test";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, input_len,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_NE(output, NULL);
    ASSERT_EQ(memcmp(output, input, input_len), 0);

    free(output);
    hl_cap_wasm_destroy(&cache);
}

/* ── Pool tests ────────────────────────────────────────────────────── */

UTEST(hl_cap_wasm, pool_reuse)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* First call: cold instantiation */
    const char *input = "pool test";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, input_len,
                               &output, &output_len,
                               NULL, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_EQ(memcmp(output, input, input_len), 0);
    free(output);

    /* Second call: should reuse pooled instance */
    output = NULL;
    output_len = 0;
    err = NULL;
    rc = hl_cap_wasm_call(&cache, "echo",
                           input, input_len,
                           &output, &output_len,
                           NULL, NULL, NULL,
                           &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_EQ(memcmp(output, input, input_len), 0);
    free(output);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, pool_stress)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *input = "stress";
    size_t input_len = strlen(input);

    for (int i = 0; i < 100; i++) {
        void *output = NULL;
        size_t output_len = 0;
        const char *err = NULL;

        int rc = hl_cap_wasm_call(&cache, "echo",
                                   input, input_len,
                                   &output, &output_len,
                                   NULL, NULL, NULL,
                                   &vfs, NULL, NULL, &err);
        ASSERT_EQ(rc, 0);
        ASSERT_EQ(output_len, input_len);
        free(output);
    }

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, pool_error_no_reuse)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* Gas-exhausted call - instance should NOT be pooled */
    HlWasmCallOpts bad_opts = {0};
    bad_opts.gas = 1;

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               "hello", 5,
                               &output, &output_len,
                               &bad_opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_NE(rc, 0);
    free(output);

    /* Normal call should still work (fresh instance) */
    output = NULL;
    output_len = 0;
    err = NULL;
    rc = hl_cap_wasm_call(&cache, "echo",
                           "hello", 5,
                           &output, &output_len,
                           NULL, NULL, NULL,
                           &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, (size_t)5);
    ASSERT_EQ(memcmp(output, "hello", 5), 0);
    free(output);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, pool_size_mismatch)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *input = "mismatch";
    size_t input_len = strlen(input);

    /* Call with heap_size = 1 MB */
    HlWasmCallOpts opts1 = {0};
    opts1.max_input  = 1024;
    opts1.max_output = 1024;
    opts1.heap_size  = 1 * 1024 * 1024;
    opts1.stack_size = 32 * 1024;

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, input_len,
                               &output, &output_len,
                               &opts1, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    free(output);

    /* Call with different heap_size = 2 MB - pool miss, fresh instance */
    HlWasmCallOpts opts2 = {0};
    opts2.max_input  = 1024;
    opts2.max_output = 1024;
    opts2.heap_size  = 2 * 1024 * 1024;
    opts2.stack_size = 32 * 1024;

    output = NULL;
    output_len = 0;
    err = NULL;
    rc = hl_cap_wasm_call(&cache, "echo",
                           input, input_len,
                           &output, &output_len,
                           &opts2, NULL, NULL,
                           &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    free(output);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, call_uninitialized_cache)
{
    HlWasmCache cache;
    memset(&cache, 0, sizeof(cache));
    /* cache.initialized is 0 - not initialized */

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               "x", 1, &output, &output_len,
                               NULL, NULL, NULL, NULL, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_INTERNAL);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "internal_error");

    /* Load on uninitialized cache */
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", NULL, NULL),
              HL_WASM_ERR_INTERNAL);
}

/* ── SIMD: Interpreter gracefully handles v128 module ──────────────── */

UTEST(hl_cap_wasm, simd_interpreter_load)
{
    /* SIMD .wasm contains v128 types. WAMR interpreter without SIMDe
     * should either: (a) load but fail at call time, or (b) fail to load.
     * Either way, it must not crash. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* Attempt to load the SIMD module */
    int rc = hl_cap_wasm_load(&cache, "simd_dot", &vfs, NULL);

    if (rc == 0) {
        /* Module loaded - try calling it. WAMR with SIMD enabled can
         * load v128 modules even in interpreter mode. Call may succeed
         * (if SIMDe is available) or fail gracefully. */
        uint8_t input[4 + 4*4 + 4*4]; /* n=4, vec_a[4], vec_b[4] */
        memset(input, 0, sizeof(input));
        uint32_t n = 4;
        memcpy(input, &n, 4);
        /* a = {1,2,3,4}, b = {1,1,1,1} */
        float a[] = {1.0f, 2.0f, 3.0f, 4.0f};
        float b[] = {1.0f, 1.0f, 1.0f, 1.0f};
        memcpy(input + 4, a, 16);
        memcpy(input + 20, b, 16);

        void *output = NULL;
        size_t output_len = 0;
        const char *err = NULL;

        rc = hl_cap_wasm_call(&cache, "simd_dot",
                               input, sizeof(input),
                               &output, &output_len,
                               NULL, NULL, NULL,
                               &vfs, NULL, NULL, &err);
        /* Either succeeds with correct result, or fails gracefully */
        if (rc == 0) {
            ASSERT_EQ(output_len, (size_t)8);
            double result;
            memcpy(&result, output, 8);
            /* dot(a,b) = 1+2+3+4 = 10 */
            ASSERT_TRUE(result > 9.99 && result < 10.01);
        }
        /* If rc != 0, that's also fine - interpreter SIMD not supported */
        free(output);
    }
    /* If load failed, that's fine - confirms interpreter can't load v128 */

    hl_cap_wasm_destroy(&cache);
}

/* ── TC-1: Concurrent module loading (TS-2 regression) ─────────────── */

typedef struct {
    HlWasmCache *cache;
    HlVfs       *vfs;
    int          rc;
} ConcurrentLoadArg;

static void *concurrent_load_thread(void *arg)
{
    ConcurrentLoadArg *a = (ConcurrentLoadArg *)arg;
    const char *input = "concurrent";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    a->rc = hl_cap_wasm_call(a->cache, "echo",
                              input, input_len,
                              &output, &output_len,
                              NULL, NULL, NULL,
                              a->vfs, NULL, NULL, &err);
    if (a->rc == 0) {
        if (output_len != input_len || memcmp(output, input, input_len) != 0)
            a->rc = -99;
    }
    free(output);
    return NULL;
}

UTEST(hl_cap_wasm, concurrent_load)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    #define NUM_THREADS 8
    pthread_t threads[NUM_THREADS];
    ConcurrentLoadArg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].cache = &cache;
        args[i].vfs = &vfs;
        args[i].rc = -1;
        pthread_create(&threads[i], NULL, concurrent_load_thread, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    /* All threads should succeed */
    for (int i = 0; i < NUM_THREADS; i++)
        ASSERT_EQ(args[i].rc, 0);

    /* Module should be cached exactly once */
    ASSERT_EQ(cache.count, 1);
    #undef NUM_THREADS

    hl_cap_wasm_destroy(&cache);
}

/* ── TC-2: Callback provided but not invoked ───────────────────────── */

static int test_callback_fn(int id, const void *in, size_t in_len,
                             void *out_buf, size_t out_max, void *user_data)
{
    (void)id; (void)in; (void)in_len; (void)out_buf; (void)out_max;
    /* Mark that callback was invoked */
    int *called = (int *)user_data;
    *called = 1;
    return 0;
}

UTEST(hl_cap_wasm, callback_not_invoked)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    int callback_called = 0;
    const char *input = "callback test";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    /* Provide a callback, but echo.wasm never invokes host_call(CALLBACK) */
    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, input_len,
                               &output, &output_len,
                               NULL, test_callback_fn, &callback_called,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(callback_called, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_NE(output, NULL);
    ASSERT_EQ(memcmp(output, input, input_len), 0);

    free(output);
    hl_cap_wasm_destroy(&cache);
}

/* ── TC-3: Gas clamping edge cases ─────────────────────────────────── */

UTEST(hl_cap_wasm, gas_clamping_edge_cases)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *input = "gas";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;
    int rc;

    /* gas = INT_MAX - should succeed (not clamped) */
    HlWasmCallOpts opts1 = {0};
    opts1.gas = INT_MAX;
    rc = hl_cap_wasm_call(&cache, "echo",
                           input, input_len,
                           &output, &output_len,
                           &opts1, NULL, NULL,
                           &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    free(output);

    /* gas = HL_WASM_MAX_GAS - should succeed (accepted as-is) */
    output = NULL;
    output_len = 0;
    err = NULL;
    HlWasmCallOpts opts2 = {0};
    opts2.gas = HL_WASM_MAX_GAS;
    rc = hl_cap_wasm_call(&cache, "echo",
                           input, input_len,
                           &output, &output_len,
                           &opts2, NULL, NULL,
                           &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    free(output);

    /* gas = HL_WASM_MAX_GAS + 1 - silently clamped, echo is fast so succeeds */
    output = NULL;
    output_len = 0;
    err = NULL;
    HlWasmCallOpts opts3 = {0};
    opts3.gas = HL_WASM_MAX_GAS + 1;
    rc = hl_cap_wasm_call(&cache, "echo",
                           input, input_len,
                           &output, &output_len,
                           &opts3, NULL, NULL,
                           &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    free(output);

    hl_cap_wasm_destroy(&cache);
}

/* ── Large allocation tests ─────────────────────────────────────────── */

/* Helper: generate deterministic input of given size and verify echo output.
 * Uses a simple PRNG to avoid allocating a second buffer for comparison -
 * we regenerate the expected bytes and compare in chunks. */
static uint32_t large_xorshift(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static void fill_deterministic(uint8_t *buf, size_t len, uint32_t seed)
{
    uint32_t state = seed;
    size_t i = 0;
    /* Fill in 4-byte chunks */
    for (; i + 4 <= len; i += 4) {
        state = large_xorshift(state);
        memcpy(buf + i, &state, 4);
    }
    /* Remaining bytes */
    if (i < len) {
        state = large_xorshift(state);
        memcpy(buf + i, &state, len - i);
    }
}

static int verify_deterministic(const uint8_t *buf, size_t len, uint32_t seed)
{
    uint32_t state = seed;
    size_t i = 0;
    for (; i + 4 <= len; i += 4) {
        state = large_xorshift(state);
        uint32_t expected;
        memcpy(&expected, buf + i, 4);
        if (expected != state) return 0;
    }
    if (i < len) {
        state = large_xorshift(state);
        uint8_t tmp[4];
        memcpy(tmp, &state, 4);
        if (memcmp(buf + i, tmp, len - i) != 0) return 0;
    }
    return 1;
}

UTEST(hl_cap_wasm, large_heap_instantiation)
{
    /* Verify that a 256 MB WASM heap can be instantiated and used.
     * This proves the raised HL_WASM_MAX_HEAP limit works end-to-end. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts opts = {0};
    opts.heap_size  = 256 * 1024 * 1024;  /* 256 MB */
    opts.max_input  = 1024;
    opts.max_output = 1024;

    const char *input = "large heap test";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, input_len,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_NE(output, NULL);
    ASSERT_EQ(memcmp(output, input, input_len), 0);

    free(output);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, large_io_32mb)
{
    /* Pass 32 MB through echo.wasm with a 128 MB heap.
     * Validates the full clamping chain and WASM linear memory
     * allocation for inputs well above the old 4 MB test ceiling. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    size_t io_size = 32 * 1024 * 1024;  /* 32 MB */
    uint8_t *input = malloc(io_size);
    ASSERT_NE(input, NULL);
    fill_deterministic(input, io_size, 0xDEADBEEF);

    HlWasmCallOpts opts = {0};
    opts.heap_size  = 128 * 1024 * 1024;  /* 128 MB - room for in + out */
    opts.max_input  = (uint32_t)io_size;
    opts.max_output = (uint32_t)io_size;

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, io_size,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, io_size);
    ASSERT_NE(output, NULL);
    /* Verify byte-exact match using deterministic pattern */
    ASSERT_TRUE(verify_deterministic(output, output_len, 0xDEADBEEF));

    free(output);
    free(input);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, large_io_128mb)
{
    /* Pass 128 MB through echo.wasm with a 512 MB heap.
     * Exercises the full path near the HL_WASM_MAX_IO_SIZE (256 MB) limit. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    size_t io_size = 128 * 1024 * 1024;  /* 128 MB */
    uint8_t *input = malloc(io_size);
    ASSERT_NE(input, NULL);
    fill_deterministic(input, io_size, 0xCAFEBABE);

    HlWasmCallOpts opts = {0};
    opts.heap_size  = 512 * 1024 * 1024;  /* 512 MB - room for in + out */
    opts.max_input  = (uint32_t)io_size;
    opts.max_output = (uint32_t)io_size;

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, io_size,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, io_size);
    ASSERT_NE(output, NULL);
    ASSERT_TRUE(verify_deterministic(output, output_len, 0xCAFEBABE));

    free(output);
    free(input);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, io_size_clamping)
{
    /* Verify that max_input/max_output beyond HL_WASM_MAX_IO_SIZE
     * gets silently clamped - the call should succeed with clamped limits. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts opts = {0};
    /* Request 512 MB I/O - exceeds HL_WASM_MAX_IO_SIZE (256 MB).
     * Should be silently clamped to 256 MB. Heap must be large enough
     * for the clamped output buffer allocation in WASM linear memory. */
    opts.max_input  = 512 * 1024 * 1024;
    opts.max_output = 512 * 1024 * 1024;
    opts.heap_size  = 768 * 1024 * 1024;  /* room for clamped 256 MB out */

    const char *input = "clamp test";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, input_len,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_EQ(memcmp(output, input, input_len), 0);

    free(output);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, heap_size_clamping)
{
    /* Request heap > HL_WASM_MAX_HEAP - should be silently clamped. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts opts = {0};
    /* UINT32_MAX heap - exceeds HL_WASM_MAX_HEAP, clamped to ~4GB.
     * WAMR may fail to allocate 4GB, so we accept either success or
     * a graceful INTERNAL error. The point is: no crash, no truncation. */
    opts.heap_size = UINT32_MAX;
    opts.max_input = 1024;
    opts.max_output = 1024;

    const char *input = "huge heap";
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, strlen(input),
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    /* Either succeeds (system has enough memory) or fails gracefully */
    if (rc == 0) {
        ASSERT_EQ(output_len, strlen(input));
        ASSERT_EQ(memcmp(output, input, strlen(input)), 0);
    } else {
        /* Graceful failure - not a crash */
        ASSERT_EQ(rc, HL_WASM_ERR_INTERNAL);
    }

    free(output);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, input_exceeds_clamped_max)
{
    /* Set max_input to 16 bytes, then pass 32 bytes.
     * Even though the user could set max_input higher, with the small
     * limit in place, the call must reject the oversized input. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts opts = {0};
    opts.max_input = 16;

    char input[32];
    memset(input, 'A', sizeof(input));
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, sizeof(input),
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_INPUT);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "input_too_large");
    ASSERT_EQ(output, NULL);

    hl_cap_wasm_destroy(&cache);
}

/* ── Persistent instance tests ──────────────────────────────────────── */

UTEST(hl_cap_wasm, instance_create_destroy)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        &cache, "echo", NULL, &vfs, NULL, NULL, &err);
    ASSERT_NE(pi, NULL);
    ASSERT_STREQ(pi->name, "echo");
    ASSERT_EQ(pi->closed, 0);

    hl_cap_wasm_instance_destroy(pi);
    /* pi is freed - don't touch it */

    hl_cap_wasm_destroy(&cache);
}

/* Audit 10: live persistent instances are bounded per cache (each reserves
 * linear memory + an exec-env stack outside the VM heap limit), and closing
 * one hands its slot back. */
UTEST(hl_cap_wasm, instance_live_count_bounded)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts small = {0};
    small.heap_size = 64 * 1024;
    small.max_input = 64;
    small.max_output = 64;
    enum { N = HL_WASM_MAX_LIVE_INSTANCES };
    static HlWasmInstance *insts[N];
    const char *err = NULL;
    for (int i = 0; i < N; i++) {
        insts[i] = hl_cap_wasm_instance_create(&cache, "echo", &small,
                                               &vfs, NULL, NULL, &err);
        ASSERT_TRUE(insts[i] != NULL);
    }
    ASSERT_EQ(cache.live_instances, N);

    err = NULL;
    ASSERT_TRUE(hl_cap_wasm_instance_create(&cache, "echo", &small,
                                            &vfs, NULL, NULL, &err) == NULL);
    ASSERT_STREQ("too_many_instances", err);
    ASSERT_EQ(cache.live_instances, N);

    hl_cap_wasm_instance_destroy(insts[0]);
    ASSERT_EQ(cache.live_instances, N - 1);
    insts[0] = hl_cap_wasm_instance_create(&cache, "echo", &small,
                                           &vfs, NULL, NULL, &err);
    ASSERT_TRUE(insts[0] != NULL);

    for (int i = 0; i < N; i++)
        hl_cap_wasm_instance_destroy(insts[i]);
    ASSERT_EQ(cache.live_instances, 0);
    ASSERT_EQ(cache.live_instance_bytes, (uint64_t)0);
    hl_cap_wasm_destroy(&cache);
}

/* The byte budget refuses a second instance past it, never the first. */
UTEST(hl_cap_wasm, instance_live_bytes_bounded)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* Pretend earlier instances hold all but 1 KB of the budget. */
    cache.live_instances = 1;
    cache.live_instance_bytes = HL_WASM_MAX_LIVE_INSTANCE_BYTES - 1024;
    HlWasmCallOpts small = {0};
    small.heap_size = 64 * 1024;
    small.max_input = 64;
    small.max_output = 64;
    const char *err = NULL;
    ASSERT_TRUE(hl_cap_wasm_instance_create(&cache, "echo", &small,
                                            &vfs, NULL, NULL, &err) == NULL);
    ASSERT_STREQ("too_many_instances", err);

    /* With none live, the same instance is allowed. */
    cache.live_instances = 0;
    cache.live_instance_bytes = 0;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(&cache, "echo", &small,
                                                     &vfs, NULL, NULL, &err);
    ASSERT_TRUE(pi != NULL);
    hl_cap_wasm_instance_destroy(pi);
    ASSERT_EQ(cache.live_instances, 0);
    hl_cap_wasm_destroy(&cache);
}

static uint64_t test_linear_pages(void *inst)
{
    wasm_memory_inst_t mem =
        wasm_runtime_get_default_memory((wasm_module_inst_t)inst);
    return mem ? (uint64_t)wasm_memory_get_cur_page_count(mem) : 0;
}

/* grow.wasm called with n input bytes: 1 when memory.grow(n) worked. */
static int test_grow_call(HlWasmInstance *pi, size_t pages)
{
    static const char ones[256] = {0};
    void *out = NULL;
    size_t out_len = 0;
    const char *err = NULL;
    int rc = hl_cap_wasm_instance_call(pi, ones, pages, &out, &out_len,
                                       NULL, NULL, NULL, NULL, &err);
    free(out);
    return rc == HL_WASM_OK ? (int)out_len : -1;
}

/* Audit 15 follow-up: a persistent instance lives across calls, outside every
 * VM heap limit, so memory.grow is capped at instantiation - it may grow by
 * its heap_size (in pages) past where it started, and the guest sees -1 past
 * that. The live-instance reservation covers that growth and is handed back
 * on destroy. */
UTEST(hl_cap_wasm, instance_memory_grow_capped_at_reservation)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts small = {0};
    small.heap_size = 64 * 1024;        /* one page of growth */
    small.max_input = 256;
    small.max_output = 64;
    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(&cache, "grow", &small,
                                                     &vfs, NULL, NULL, &err);
    ASSERT_TRUE(pi != NULL);
    ASSERT_EQ(cache.live_instances, 1);
    ASSERT_EQ(cache.live_instance_bytes,
              hl_wasm_instance_reservation(64 * 1024, HL_WASM_DEFAULT_STACK));

    uint64_t start = test_linear_pages(pi->instance);
    ASSERT_TRUE(start > 0);

    /* Far past the reservation: refused inside the guest, nothing grown. */
    ASSERT_EQ(test_grow_call(pi, 200), 0);
    ASSERT_EQ(test_linear_pages(pi->instance), start);

    /* The one page it reserved: granted. */
    ASSERT_EQ(test_grow_call(pi, 1), 1);
    ASSERT_EQ(test_linear_pages(pi->instance), start + 1);

    /* And not one more, on this call or any later one. */
    ASSERT_EQ(test_grow_call(pi, 1), 0);
    ASSERT_EQ(test_grow_call(pi, 1), 0);
    ASSERT_EQ(test_linear_pages(pi->instance), start + 1);

    hl_cap_wasm_instance_destroy(pi);
    ASSERT_EQ(cache.live_instances, 0);
    ASSERT_EQ(cache.live_instance_bytes, (uint64_t)0);
    hl_cap_wasm_destroy(&cache);
}

/* The cap is the persistent instance's own: a pooled one-shot call still
 * grows its memory as far as the module allows. */
UTEST(hl_cap_wasm, pooled_call_memory_grow_not_capped)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts small = {0};
    small.heap_size = 64 * 1024;
    small.max_input = 256;
    small.max_output = 64;
    static const char ones[64] = {0};
    void *out = NULL;
    size_t out_len = 0;
    const char *err = NULL;
    ASSERT_EQ(hl_cap_wasm_call(&cache, "grow", ones, sizeof ones,
                               &out, &out_len, &small, NULL, NULL,
                               &vfs, NULL, NULL, &err), HL_WASM_OK);
    ASSERT_EQ(out_len, (size_t)1);
    free(out);
    hl_cap_wasm_destroy(&cache);
}

/* Audit 29: pooling went by the heap_size asked for, never the memory the
 * instance holds after the call, so a guest that grew its memory was pooled
 * holding it. An instance that grew past its heap is destroyed instead; one
 * that did not is pooled as before - on the copy path and on the zero-copy
 * release alike. */
UTEST(hl_cap_wasm, grown_instance_is_not_pooled)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts small = {0};
    small.heap_size = 64 * 1024;
    small.max_input = 256;
    small.max_output = 64;
    static const char ones[64] = {0};   /* grows 64 pages = 4 MB */
    void *out = NULL;
    size_t out_len = 0;
    const char *err = NULL;

    /* No growth (0 input bytes): pooled. */
    ASSERT_EQ(hl_cap_wasm_call(&cache, "grow", ones, 0, &out, &out_len,
                               &small, NULL, NULL, &vfs, NULL, NULL, &err),
              HL_WASM_OK);
    free(out); out = NULL;
    HlWasmModule *mod = hl_cap_wasm_module_lookup(&cache, "grow");
    ASSERT_TRUE(mod != NULL);
    ASSERT_TRUE(mod->probe_linear_bytes > 0);
    ASSERT_EQ(mod->pool.count, 1);

    /* That pooled instance grows 64 pages: destroyed, not pooled again. */
    ASSERT_EQ(hl_cap_wasm_call(&cache, "grow", ones, sizeof ones, &out,
                               &out_len, &small, NULL, NULL, &vfs, NULL, NULL,
                               &err), HL_WASM_OK);
    free(out); out = NULL;
    ASSERT_EQ(out_len, (size_t)1);
    ASSERT_EQ(mod->pool.count, 0);

    /* The zero-copy release takes the same path. */
    HlWasmBuffer *b = NULL;
    ASSERT_EQ(hl_cap_wasm_call_buf(&cache, "grow", ones, sizeof ones, &b,
                                   &small, NULL, NULL, &vfs, NULL, NULL, &err),
              HL_WASM_OK);
    ASSERT_TRUE(b != NULL);
    hl_wasm_buffer_close(b);
    ASSERT_EQ(mod->pool.count, 0);
    hl_cap_wasm_destroy(&cache);
}

/* Audit 15 follow-up: a zero-copy buffer keeps a pooled instance checked out;
 * it reserves the linear memory the instance holds when it is checked out
 * (memory.grow during the call included), not just its heap + stack, and the
 * buffer's close hands exactly that back. */
UTEST(hl_cap_wasm, zero_copy_checkout_reserves_grown_memory)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts small = {0};
    small.heap_size = 64 * 1024;
    small.max_input = 256;
    small.max_output = 64;
    static const char ones[64] = {0};   /* grows 64 pages = 4 MB */
    HlWasmBuffer *b = NULL;
    const char *err = NULL;
    ASSERT_EQ(hl_cap_wasm_call_buf(&cache, "grow", ones, sizeof ones, &b,
                                   &small, NULL, NULL, &vfs, NULL, NULL, &err),
              HL_WASM_OK);
    ASSERT_TRUE(b != NULL);
    ASSERT_EQ((int)b->kind, (int)HL_WASM_BUF_WASM);
    ASSERT_EQ(hl_wasm_buffer_len(b), (size_t)1);
    ASSERT_EQ(cache.live_instances, 1);

    uint64_t held = test_linear_pages(b->u.wasm.instance) * HL_WASM_PAGE_BYTES;
    ASSERT_TRUE(held >= (uint64_t)65 * HL_WASM_PAGE_BYTES);
    ASSERT_EQ(cache.live_instance_bytes, held + HL_WASM_DEFAULT_STACK);
    ASSERT_EQ((uint64_t)hl_wasm_buffer_footprint(b), cache.live_instance_bytes);

    hl_wasm_buffer_close(b);
    ASSERT_EQ(cache.live_instances, 0);
    ASSERT_EQ(cache.live_instance_bytes, (uint64_t)0);
    hl_cap_wasm_destroy(&cache);
}

/* Audit 18: a guest returning more bytes than max_output is an output error
 * on every call path, not an OK with an empty output. */
UTEST(hl_cap_wasm, output_over_max_output_is_an_error)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts o = {0};
    o.max_output = 64;
    const char *err = NULL;

    HlWasmBuffer *b = NULL;
    ASSERT_EQ(hl_cap_wasm_call_buf(&cache, "overrun", "x", 1, &b, &o,
                                   NULL, NULL, &vfs, NULL, NULL, &err),
              HL_WASM_ERR_OUTPUT);
    ASSERT_TRUE(b == NULL);
    ASSERT_STREQ(err, "output_too_small");
    ASSERT_EQ(cache.live_instances, 0);

    void *out = NULL;
    size_t out_len = 0;
    err = NULL;
    ASSERT_EQ(hl_cap_wasm_call(&cache, "overrun", "x", 1, &out, &out_len, &o,
                               NULL, NULL, &vfs, NULL, NULL, &err),
              HL_WASM_ERR_OUTPUT);
    ASSERT_TRUE(out == NULL);
    ASSERT_EQ(out_len, (size_t)0);

    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        &cache, "overrun", &o, &vfs, NULL, NULL, &err);
    ASSERT_NE(pi, NULL);
    b = NULL;
    err = NULL;
    ASSERT_EQ(hl_cap_wasm_instance_call_buf(pi, "x", 1, &b, &o, NULL, NULL,
                                            NULL, &err),
              HL_WASM_ERR_OUTPUT);
    ASSERT_TRUE(b == NULL);
    ASSERT_STREQ(err, "output_too_small");
    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, instance_call_echo)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        &cache, "echo", NULL, &vfs, NULL, NULL, &err);
    ASSERT_NE(pi, NULL);

    void *output = NULL;
    size_t output_len = 0;
    int rc = hl_cap_wasm_instance_call(pi, "hello", 5,
                                        &output, &output_len,
                                        NULL, NULL, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(output_len, (size_t)5);
    ASSERT_EQ(memcmp(output, "hello", 5), 0);
    free(output);

    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, instance_repeated_calls)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        &cache, "echo", NULL, &vfs, NULL, NULL, &err);
    ASSERT_NE(pi, NULL);

    /* 100 calls on the same instance */
    for (int i = 0; i < 100; i++) {
        void *output = NULL;
        size_t output_len = 0;
        int rc = hl_cap_wasm_instance_call(pi, "test", 4,
                                            &output, &output_len,
                                            NULL, NULL, NULL, NULL, &err);
        ASSERT_EQ(rc, HL_WASM_OK);
        ASSERT_EQ(output_len, (size_t)4);
        ASSERT_EQ(memcmp(output, "test", 4), 0);
        free(output);
    }

    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, instance_gas_recovery)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        &cache, "echo", NULL, &vfs, NULL, NULL, &err);
    ASSERT_NE(pi, NULL);

    /* Gas = 1 should fail */
    HlWasmCallOpts low_gas = {0};
    low_gas.gas = 1;
    void *output = NULL;
    size_t output_len = 0;
    int rc = hl_cap_wasm_instance_call(pi, "x", 1,
                                        &output, &output_len,
                                        &low_gas, NULL, NULL, NULL, &err);
    /* Instance path must classify gas exhaustion the same way (#314). */
    ASSERT_EQ(rc, HL_WASM_ERR_GAS);
    ASSERT_TRUE(err != NULL);
    ASSERT_STREQ(err, "gas_exhausted");

    /* Now with normal gas - should succeed (instance reusable) */
    HlWasmCallOpts normal_gas = {0};
    normal_gas.gas = 10000000;
    output = NULL;
    output_len = 0;
    err = NULL;
    rc = hl_cap_wasm_instance_call(pi, "recover", 7,
                                    &output, &output_len,
                                    &normal_gas, NULL, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(output_len, (size_t)7);
    ASSERT_EQ(memcmp(output, "recover", 7), 0);
    free(output);

    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, instance_closed_rejects)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        &cache, "echo", NULL, &vfs, NULL, NULL, &err);
    ASSERT_NE(pi, NULL);

    /* Close it, then try to call */
    pi->closed = 1;
    void *output = NULL;
    size_t output_len = 0;
    int rc = hl_cap_wasm_instance_call(pi, "x", 1,
                                        &output, &output_len,
                                        NULL, NULL, NULL, NULL, &err);
    ASSERT_NE(rc, HL_WASM_OK);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "instance_closed");

    /* Clean up without destroy (already logically closed, but not freed) */
    pi->closed = 0; /* undo for proper destroy */
    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, instance_not_found)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        &cache, "nonexistent", NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(pi, NULL);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "not_found");

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, instance_call_buf)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        &cache, "echo", NULL, &vfs, NULL, NULL, &err);
    ASSERT_NE(pi, NULL);

    HlWasmBuffer *buf = NULL;
    int rc = hl_cap_wasm_instance_call_buf(pi, "buftest", 7,
                                            &buf, NULL, NULL, NULL,
                                            NULL, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_NE(buf, NULL);
    ASSERT_EQ(hl_wasm_buffer_len(buf), (size_t)7);
    ASSERT_EQ(memcmp(hl_wasm_buffer_data(buf), "buftest", 7), 0);

    hl_wasm_buffer_destroy(buf);
    free(buf);

    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, instance_busy_rejects)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        &cache, "echo", NULL, &vfs, NULL, NULL, &err);
    ASSERT_NE(pi, NULL);

    /* Manually set busy */
    atomic_store(&pi->busy, 1);

    void *output = NULL;
    size_t output_len = 0;
    err = NULL;
    int rc = hl_cap_wasm_instance_call(pi, "x", 1,
                                        &output, &output_len,
                                        NULL, NULL, NULL, NULL, &err);
    ASSERT_NE(rc, HL_WASM_OK);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "instance_busy");

    atomic_store(&pi->busy, 0);
    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
}

/* ── Stateful kv_store tests ──────────────────────────────────────── */

/* Helper: build a LOAD message for kv_store.
 * Format: [0x01] [count:u32] [key_len:u32 key... val_len:u32 val...]... */
static uint8_t *build_kv_load(int count, const char **keys, const char **vals,
                               size_t *out_len)
{
    /* Calculate total size */
    size_t total = 1 + 4; /* opcode + count */
    for (int i = 0; i < count; i++)
        total += 4 + strlen(keys[i]) + 4 + strlen(vals[i]);

    uint8_t *buf = malloc(total);
    if (!buf) return NULL;

    uint8_t *p = buf;
    *p++ = 0x01; /* LOAD opcode */
    uint32_t cnt = (uint32_t)count;
    memcpy(p, &cnt, 4); p += 4;

    for (int i = 0; i < count; i++) {
        uint32_t klen = (uint32_t)strlen(keys[i]);
        memcpy(p, &klen, 4); p += 4;
        memcpy(p, keys[i], klen); p += klen;
        uint32_t vlen = (uint32_t)strlen(vals[i]);
        memcpy(p, &vlen, 4); p += 4;
        memcpy(p, vals[i], vlen); p += vlen;
    }

    *out_len = total;
    return buf;
}

/* Helper: build a GET message: [0x02] [key bytes] */
static uint8_t *build_kv_get(const char *key, size_t *out_len)
{
    size_t klen = strlen(key);
    *out_len = 1 + klen;
    uint8_t *buf = malloc(*out_len);
    if (!buf) return NULL;
    buf[0] = 0x02;
    memcpy(buf + 1, key, klen);
    return buf;
}

/* Helper: build a COUNT message: [0x03] */
static uint8_t *build_kv_count(size_t *out_len)
{
    *out_len = 1;
    uint8_t *buf = malloc(1);
    if (!buf) return NULL;
    buf[0] = 0x03;
    return buf;
}

UTEST(hl_cap_wasm, instance_kv_store_stateful)
{
    /* This test proves persistent instances retain state across calls:
     * 1. Create persistent kv_store instance
     * 2. LOAD 3 key-value pairs
     * 3. GET each key → verify correct value returned
     * 4. COUNT → verify 3
     * 5. GET missing key → verify 0 bytes returned */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        &cache, "kv_store", NULL, &vfs, NULL, NULL, &err);
    ASSERT_NE(pi, NULL);

    /* LOAD 3 entries */
    const char *keys[] = { "name", "lang", "version" };
    const char *vals[] = { "hull", "c11", "1.0.0" };
    size_t load_len;
    uint8_t *load_msg = build_kv_load(3, keys, vals, &load_len);
    ASSERT_NE(load_msg, NULL);

    void *output = NULL;
    size_t output_len = 0;
    int rc = hl_cap_wasm_instance_call(pi, load_msg, load_len,
                                        &output, &output_len,
                                        NULL, NULL, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(output_len, (size_t)4);
    uint32_t loaded_count;
    memcpy(&loaded_count, output, 4);
    ASSERT_EQ(loaded_count, (uint32_t)3);
    free(output);
    free(load_msg);

    /* GET "name" → "hull" */
    size_t get_len;
    uint8_t *get_msg = build_kv_get("name", &get_len);
    output = NULL; output_len = 0; err = NULL;
    rc = hl_cap_wasm_instance_call(pi, get_msg, get_len,
                                    &output, &output_len,
                                    NULL, NULL, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(output_len, (size_t)4);
    ASSERT_EQ(memcmp(output, "hull", 4), 0);
    free(output);
    free(get_msg);

    /* GET "lang" → "c11" */
    get_msg = build_kv_get("lang", &get_len);
    output = NULL; output_len = 0; err = NULL;
    rc = hl_cap_wasm_instance_call(pi, get_msg, get_len,
                                    &output, &output_len,
                                    NULL, NULL, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(output_len, (size_t)3);
    ASSERT_EQ(memcmp(output, "c11", 3), 0);
    free(output);
    free(get_msg);

    /* GET "version" → "1.0.0" */
    get_msg = build_kv_get("version", &get_len);
    output = NULL; output_len = 0; err = NULL;
    rc = hl_cap_wasm_instance_call(pi, get_msg, get_len,
                                    &output, &output_len,
                                    NULL, NULL, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(output_len, (size_t)5);
    ASSERT_EQ(memcmp(output, "1.0.0", 5), 0);
    free(output);
    free(get_msg);

    /* COUNT → 3 */
    size_t count_len;
    uint8_t *count_msg = build_kv_count(&count_len);
    output = NULL; output_len = 0; err = NULL;
    rc = hl_cap_wasm_instance_call(pi, count_msg, count_len,
                                    &output, &output_len,
                                    NULL, NULL, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(output_len, (size_t)4);
    uint32_t count;
    memcpy(&count, output, 4);
    ASSERT_EQ(count, (uint32_t)3);
    free(output);
    free(count_msg);

    /* GET missing key → 0 bytes */
    get_msg = build_kv_get("missing", &get_len);
    output = NULL; output_len = 0; err = NULL;
    rc = hl_cap_wasm_instance_call(pi, get_msg, get_len,
                                    &output, &output_len,
                                    NULL, NULL, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(output_len, (size_t)0);
    free(output);
    free(get_msg);

    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, instance_kv_store_unpooled_loses_state)
{
    /* Proves that unpooled calls do NOT retain state:
     * LOAD data with one call, then GET with a fresh call → not found.
     * Uses heap > pool threshold (4 MB) to force fresh instance per call.
     * This is the contrast case showing why persistent instances matter. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* Use large heap to bypass instance pool */
    HlWasmCallOpts opts = {0};
    opts.heap_size = 8 * 1024 * 1024; /* 8 MB > 4 MB pool threshold */

    /* LOAD via unpooled call */
    const char *keys[] = { "key1" };
    const char *vals[] = { "val1" };
    size_t load_len;
    uint8_t *load_msg = build_kv_load(1, keys, vals, &load_len);

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;
    int rc = hl_cap_wasm_call(&cache, "kv_store",
                               load_msg, load_len,
                               &output, &output_len,
                               &opts, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    free(output);
    free(load_msg);

    /* GET via unpooled call - fresh instance, state lost */
    size_t get_len;
    uint8_t *get_msg = build_kv_get("key1", &get_len);
    output = NULL; output_len = 0; err = NULL;
    rc = hl_cap_wasm_call(&cache, "kv_store",
                           get_msg, get_len,
                           &output, &output_len,
                           &opts, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    /* Key not found because fresh instance has no state */
    ASSERT_EQ(output_len, (size_t)0);
    free(output);
    free(get_msg);

    hl_cap_wasm_destroy(&cache);
}

/* ── Shared data tests ─────────────────────────────────────────────── */

/* Helper: build shared_read input message.
 * segment_id, offset (u32 LE), length (u32 LE) = 9 bytes total.
 * For count query: segment_id=0xFF, 0 bytes for offset/length. */
static uint8_t *build_shared_read_msg(int segment_id, uint32_t offset,
                                       uint32_t length, size_t *out_len)
{
    if (segment_id == 0xFF) {
        *out_len = 1;
        uint8_t *buf = malloc(1);
        buf[0] = 0xFF;
        return buf;
    }
    *out_len = 9;
    uint8_t *buf = malloc(9);
    buf[0] = (uint8_t)segment_id;
    memcpy(buf + 1, &offset, 4);
    memcpy(buf + 5, &length, 4);
    return buf;
}

UTEST(hl_cap_wasm, shared_data_load_unload)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    const char data[] = "hello shared world";

    /* Load a segment */
    int rc = hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
                                    data, sizeof(data) - 1, NULL,
                                    &vfs, NULL, &err);
    ASSERT_EQ(rc, 0);

    /* Verify shared_data is set on module */
    HlWasmModule *mod = NULL;
    pthread_mutex_lock(&cache.pool_mutex);
    for (int i = 0; i < cache.count; i++) {
        if (strcmp(cache.modules[i].name, "shared_read") == 0) {
            mod = &cache.modules[i];
            break;
        }
    }
    pthread_mutex_unlock(&cache.pool_mutex);
    ASSERT_NE(mod, NULL);
    ASSERT_NE(mod->shared_data, NULL);
    ASSERT_EQ(mod->shared_data->count, 1);

    /* Remove all (NULL segment + NULL data) */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", NULL, NULL, 0, NULL,
                                    &vfs, NULL, &err), 0);
    ASSERT_EQ(mod->shared_data, NULL);

    hl_cap_wasm_destroy(&cache);
}

/* A zero-copy segment holds the caller's pin until its descriptor is gone,
 * and only then releases it; copied data, or a failed load, releases at
 * once. Without the pin, mmap.close() unmapped memory the shared heap still
 * pointed at. */
static int g_pin_releases;
static void count_pin_release(void *pin) { (void)pin; g_pin_releases++; }

UTEST(hl_cap_wasm, segment_pin_lives_as_long_as_the_segment)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    void *mem = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_TRUE(mem != MAP_FAILED);
    memset(mem, 'x', (size_t)page);
    const char *err = NULL;
    int pin = 0;

    /* Page-sized: used in place, so the pin is kept... */
    g_pin_releases = 0;
    ASSERT_EQ(hl_cap_wasm_data_load_pinned(&cache, "shared_read", "seg0",
                                           mem, (size_t)page, mem, &vfs, NULL,
                                           &err, &pin, count_pin_release), 0);
    EXPECT_EQ(g_pin_releases, 0);
    /* ...until the segment is removed. */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
                                    NULL, 0, NULL, &vfs, NULL, &err), 0);
    EXPECT_EQ(g_pin_releases, 1);

    /* Not page-sized: copied, so the pin is released straight away. */
    g_pin_releases = 0;
    ASSERT_EQ(hl_cap_wasm_data_load_pinned(&cache, "shared_read", "seg1",
                                           mem, 10, mem, &vfs, NULL,
                                           &err, &pin, count_pin_release), 0);
    EXPECT_EQ(g_pin_releases, 1);

    /* A failed load releases it too. */
    g_pin_releases = 0;
    EXPECT_NE(hl_cap_wasm_data_load_pinned(&cache, "no_such_module", "seg0",
                                           mem, (size_t)page, mem, &vfs, NULL,
                                           &err, &pin, count_pin_release), 0);
    EXPECT_EQ(g_pin_releases, 1);

    /* Teardown releases a pin still held. */
    g_pin_releases = 0;
    ASSERT_EQ(hl_cap_wasm_data_load_pinned(&cache, "shared_read", "seg2",
                                           mem, (size_t)page, mem, &vfs, NULL,
                                           &err, &pin, count_pin_release), 0);
    hl_cap_wasm_destroy(&cache);
    EXPECT_EQ(g_pin_releases, 1);
    munmap(mem, (size_t)page);
}

/* ── R1: segment mutation refused while async compute calls in flight ──
 *
 * A worker thread holds a snapshot of mod->shared_data / chain_head taken
 * under mod->mutex (pool_acquire / the persistent path) and dereferences
 * it unlocked during its gas-metered call. The event-loop-side
 * hl_cap_wasm_data_load must NOT free/rebuild those segments while a call
 * is outstanding, or the worker reads freed memory. The guard is the
 * per-module inflight_async counter, bumped at async submit and cleared
 * at completion (both event-loop-thread). */

UTEST(hl_cap_wasm, data_load_refused_while_async_in_flight)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    const char data[] = "segment-bytes";

    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
                                    data, sizeof(data) - 1, NULL,
                                    &vfs, NULL, &err), 0);

    HlWasmModule *mod = hl_cap_wasm_module_lookup(&cache, "shared_read");
    ASSERT_NE(mod, NULL);
    ASSERT_EQ(mod->inflight_async, 0);       /* counter starts clear */
    ASSERT_NE(mod->shared_data, NULL);
    ASSERT_EQ(mod->shared_data->count, 1);

    /* Reserve an in-flight slot, exactly as hl_worker_wasm_submit does. */
    mod->inflight_async = 1;

    /* All three mutation forms must refuse and leave the segment intact. */
    err = NULL;                              /* add/replace */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "seg1",
                                    data, sizeof(data) - 1, NULL,
                                    &vfs, NULL, &err), -1);
    ASSERT_STREQ(err, "async_calls_in_flight");

    err = NULL;                              /* remove one */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
                                    NULL, 0, NULL, &vfs, NULL, &err), -1);
    ASSERT_STREQ(err, "async_calls_in_flight");

    err = NULL;                              /* remove all */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", NULL,
                                    NULL, 0, NULL, &vfs, NULL, &err), -1);
    ASSERT_STREQ(err, "async_calls_in_flight");

    ASSERT_NE(mod->shared_data, NULL);       /* nothing was freed */
    ASSERT_EQ(mod->shared_data->count, 1);

    /* Once the call completes, mutation is allowed again. */
    mod->inflight_async = 0;
    err = NULL;
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", NULL,
                                    NULL, 0, NULL, &vfs, NULL, &err), 0);
    ASSERT_EQ(mod->shared_data, NULL);

    hl_cap_wasm_destroy(&cache);
}

/* Cross-thread regression test for R1: a worker thread holds a real
 * shared_data snapshot (as pool_acquire hands to the gas-metered call)
 * while the event loop attempts to free the segment. The guard must
 * refuse the free, so the worker's snapshot stays valid. Run under TSan
 * to prove no data race, and under ASan to prove no use-after-free; both
 * would fire if the guard regressed and the free proceeded. */
typedef struct {
    HlWasmModule *mod;
    atomic_int    snapshotted;
    atomic_int    release;
    int           observed_count;
} SnapHoldCtx;

static void *snapshot_holder(void *arg)
{
    SnapHoldCtx *c = (SnapHoldCtx *)arg;
    /* Snapshot under the module mutex, exactly like pool_acquire. */
    pthread_mutex_lock(&c->mod->mutex);
    const HlWasmSharedData *sd = c->mod->shared_data;
    pthread_mutex_unlock(&c->mod->mutex);

    atomic_store(&c->snapshotted, 1);

    /* Dereference the held snapshot until released. */
    int last = -1;
    while (!atomic_load(&c->release))
        if (sd) last = sd->count;
    if (sd) last = sd->count;
    c->observed_count = last;
    return NULL;
}

UTEST(hl_cap_wasm, data_load_free_blocked_while_worker_holds_snapshot)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    const char data[] = "ABCDEFGHIJ";
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
                                    data, sizeof(data) - 1, NULL,
                                    &vfs, NULL, &err), 0);

    HlWasmModule *mod = hl_cap_wasm_module_lookup(&cache, "shared_read");
    ASSERT_NE(mod, NULL);

    /* Reserve the in-flight slot as the event loop would at submit. */
    mod->inflight_async = 1;

    SnapHoldCtx ctx = { .mod = mod, .snapshotted = 0, .release = 0,
                        .observed_count = -1 };
    pthread_t th;
    ASSERT_EQ(pthread_create(&th, NULL, snapshot_holder, &ctx), 0);

    while (!atomic_load(&ctx.snapshotted)) { /* wait for the snapshot */ }

    /* Event loop tries to free the held segment - must be refused. */
    err = NULL;
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", NULL,
                                    NULL, 0, NULL, &vfs, NULL, &err), -1);
    ASSERT_STREQ(err, "async_calls_in_flight");

    atomic_store(&ctx.release, 1);
    pthread_join(th, NULL);
    ASSERT_EQ(ctx.observed_count, 1);        /* snapshot stayed valid */

    /* Free now succeeds with no call in flight. */
    mod->inflight_async = 0;
    err = NULL;
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", NULL,
                                    NULL, 0, NULL, &vfs, NULL, &err), 0);

    hl_cap_wasm_destroy(&cache);
}

/* Shared heap probe removed - the root cause was using invokeNative_general.c
 * (32-bit generic invoker) on 64-bit platforms, which mangled native function
 * arguments. Fixed by using platform-specific assembly invokers (em64_simd.s
 * on x86_64, aarch64_simd.s on aarch64). */

UTEST(hl_cap_wasm, shared_data_call_reads)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    const char data[] = "ABCDEFGHIJ";  /* 10 bytes */

    int rc = hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
                                    data, 10, NULL, &vfs, NULL, &err);
    ASSERT_EQ(rc, 0);

    /* Read 5 bytes starting at offset 2 from segment 0 */
    size_t msg_len;
    uint8_t *msg = build_shared_read_msg(0, 2, 5, &msg_len);
    void *output = NULL;
    size_t output_len = 0;

    rc = hl_cap_wasm_call(&cache, "shared_read",
                           msg, msg_len, &output, &output_len,
                           NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, (size_t)5);
    ASSERT_EQ(memcmp(output, "CDEFG", 5), 0);

    free(output);
    free(msg);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, shared_data_multi_segment)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    const char data_a[] = "AAAA";  /* 4 bytes */
    const char data_b[] = "BBBBBB";  /* 6 bytes */
    const char data_c[] = "CC";  /* 2 bytes */

    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "a",
              data_a, 4, NULL, &vfs, NULL, &err), 0);
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "b",
              data_b, 6, NULL, &vfs, NULL, &err), 0);
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "c",
              data_c, 2, NULL, &vfs, NULL, &err), 0);

    /* Read all bytes from each segment */
    for (int seg = 0; seg < 3; seg++) {
        const char *expected[] = { "AAAA", "BBBBBB", "CC" };
        size_t expected_len[] = { 4, 6, 2 };

        size_t msg_len;
        uint8_t *msg = build_shared_read_msg(seg, 0,
                        (uint32_t)expected_len[seg], &msg_len);
        void *output = NULL;
        size_t output_len = 0;

        int rc = hl_cap_wasm_call(&cache, "shared_read",
                                   msg, msg_len, &output, &output_len,
                                   NULL, NULL, NULL, &vfs, NULL, NULL, &err);
        ASSERT_EQ(rc, 0);
        ASSERT_EQ(output_len, expected_len[seg]);
        ASSERT_EQ(memcmp(output, expected[seg], expected_len[seg]), 0);

        free(output);
        free(msg);
    }

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, shared_data_replace_segment)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    const char data1[] = "OLD_DATA";
    const char data2[] = "NEW";

    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "x",
              data1, 8, NULL, &vfs, NULL, &err), 0);

    /* Replace with new data */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "x",
              data2, 3, NULL, &vfs, NULL, &err), 0);

    /* Read from replaced segment */
    size_t msg_len;
    uint8_t *msg = build_shared_read_msg(0, 0, 3, &msg_len);
    void *output = NULL;
    size_t output_len = 0;

    int rc = hl_cap_wasm_call(&cache, "shared_read",
                               msg, msg_len, &output, &output_len,
                               NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, (size_t)3);
    ASSERT_EQ(memcmp(output, "NEW", 3), 0);

    free(output);
    free(msg);
    hl_cap_wasm_destroy(&cache);
}

/* ── Audit 15: a segment over WAMR's shared-heap limit ───────────────
 *
 * WAMR refuses a shared heap over APP_HEAP_SIZE_MAX (1 GiB) while Hull
 * allowed a segment up to the 3 GB total, so such a load always failed - and
 * a REPLACE had already freed the old segment by then: a lone segment's
 * chain_head kept naming the freed heap (the next call attached it), and with
 * more segments the slot was left hollow. The segment is now refused up
 * front, and every failure leaves the old segments as they were. */

/* Read `len` bytes of segment `seg` through the guest; 0 when they match. */
static int read_segment_matches(HlWasmCache *cache, HlVfs *vfs, int seg,
                                const char *expect, size_t len)
{
    size_t msg_len;
    uint8_t *msg = build_shared_read_msg(seg, 0, (uint32_t)len, &msg_len);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;
    int rc = hl_cap_wasm_call(cache, "shared_read", msg, msg_len,
                              &output, &output_len, NULL, NULL, NULL, vfs,
                              NULL, NULL, &err);
    int ok = rc == 0 && output_len == len && memcmp(output, expect, len) == 0;
    free(output);
    free(msg);
    return ok ? 0 : -1;
}

static HlWasmModule *find_module(HlWasmCache *cache, const char *name)
{
    HlWasmModule *mod = NULL;
    pthread_mutex_lock(&cache->pool_mutex);
    for (int i = 0; i < cache->count; i++) {
        if (strcmp(cache->modules[i].name, name) == 0) {
            mod = &cache->modules[i];
            break;
        }
    }
    pthread_mutex_unlock(&cache->pool_mutex);
    return mod;
}

UTEST(hl_cap_wasm, segment_over_wamr_limit_refused_old_segment_kept)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);
    const char *err = NULL;

    /* Oversize data: an untouched anonymous reservation (zero pages), so the
     * test commits no memory - the refusal comes before any copy. */
    size_t big = (size_t)HL_WASM_MAX_SEGMENT_SIZE + 1;
    void *mem = mmap(NULL, big, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_TRUE(mem != MAP_FAILED);

    /* No segment yet: refused, and the module is left with none. */
    err = NULL;
    EXPECT_NE(hl_cap_wasm_data_load(&cache, "shared_read", "x", mem, big,
                                    NULL, &vfs, NULL, &err), 0);
    EXPECT_TRUE(err && strcmp(err, "segment_too_large") == 0);
    HlWasmModule *mod = find_module(&cache, "shared_read");
    ASSERT_NE(mod, NULL);
    EXPECT_EQ(mod->shared_data, NULL);

    /* One segment, replaced by an oversize one: refused, the old one stays
     * loaded and is what the chain names - a later call reads it. */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "x", "OLD", 3,
                                    NULL, &vfs, NULL, &err), 0);
    err = NULL;
    EXPECT_NE(hl_cap_wasm_data_load(&cache, "shared_read", "x", mem, big,
                                    NULL, &vfs, NULL, &err), 0);
    EXPECT_TRUE(err && strcmp(err, "segment_too_large") == 0);
    ASSERT_NE(mod->shared_data, NULL);
    ASSERT_EQ(mod->shared_data->count, 1);
    EXPECT_TRUE(mod->shared_data->chain_head ==
                mod->shared_data->segments[0].shared_heap);
    EXPECT_EQ(read_segment_matches(&cache, &vfs, 0, "OLD", 3), 0);

    /* Zero-copy (page-aligned pre_alloc) oversize: refused the same way. */
    EXPECT_NE(hl_cap_wasm_data_load(&cache, "shared_read", "x", mem,
                                    big - 1 + (size_t)sysconf(_SC_PAGESIZE),
                                    mem, &vfs, NULL, &err), 0);
    EXPECT_EQ(read_segment_matches(&cache, &vfs, 0, "OLD", 3), 0);

    /* Two segments: replacing the second, or adding a third, oversize is
     * refused and both stay chained and readable (no hollow slot). */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "y", "WHY!", 4,
                                    NULL, &vfs, NULL, &err), 0);
    EXPECT_NE(hl_cap_wasm_data_load(&cache, "shared_read", "y", mem, big,
                                    NULL, &vfs, NULL, &err), 0);
    EXPECT_NE(hl_cap_wasm_data_load(&cache, "shared_read", "z", mem, big,
                                    NULL, &vfs, NULL, &err), 0);
    ASSERT_EQ(mod->shared_data->count, 2);
    EXPECT_NE(mod->shared_data->chain_head, NULL);
    EXPECT_EQ(read_segment_matches(&cache, &vfs, 0, "OLD", 3), 0);
    EXPECT_EQ(read_segment_matches(&cache, &vfs, 1, "WHY!", 4), 0);

    /* And a normal replace still works afterwards. */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "x", "NEW", 3,
                                    NULL, &vfs, NULL, &err), 0);
    EXPECT_EQ(read_segment_matches(&cache, &vfs, 0, "NEW", 3), 0);
    EXPECT_EQ(read_segment_matches(&cache, &vfs, 1, "WHY!", 4), 0);

    hl_cap_wasm_destroy(&cache);
    munmap(mem, big);
}

/* HL_WASM_MAX_SEGMENT_SIZE must track WAMR's own limit: a heap of exactly
 * that size is accepted, one page more is refused (a WAMR upgrade that
 * changes APP_HEAP_SIZE_MAX trips this). The backing is an untouched
 * reservation: creating a pre-allocated heap does not touch it. */
UTEST(hl_cap_wasm, segment_size_cap_matches_wamr_limit)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    size_t cap = (size_t)HL_WASM_MAX_SEGMENT_SIZE;
    size_t len = cap + (size_t)page;
    void *mem = mmap(NULL, len, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_TRUE(mem != MAP_FAILED);

    SharedHeapInitArgs a;
    memset(&a, 0, sizeof(a));
    a.pre_allocated_addr = mem;
    a.size = (uint32_t)cap;
    wasm_shared_heap_t h = wasm_runtime_create_shared_heap(&a);
    EXPECT_NE(h, NULL);
    if (h)
        EXPECT_TRUE(wasm_runtime_destroy_shared_heap(h));

    a.size = (uint32_t)len;
    h = wasm_runtime_create_shared_heap(&a);
    EXPECT_EQ(h, NULL);
    if (h)
        wasm_runtime_destroy_shared_heap(h);

    munmap(mem, len);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, shared_data_remove_segment)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    const char data1[] = "AAAA";
    const char data2[] = "BBBB";

    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "a",
              data1, 4, NULL, &vfs, NULL, &err), 0);
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "b",
              data2, 4, NULL, &vfs, NULL, &err), 0);

    /* Remove segment "a" */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "a",
              NULL, 0, NULL, &vfs, NULL, &err), 0);

    /* Segment count should be 1 */
    size_t msg_len;
    uint8_t *msg = build_shared_read_msg(0xFF, 0, 0, &msg_len);
    void *output = NULL;
    size_t output_len = 0;

    int rc = hl_cap_wasm_call(&cache, "shared_read",
                               msg, msg_len, &output, &output_len,
                               NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, (size_t)4);
    uint32_t count;
    memcpy(&count, output, 4);
    ASSERT_EQ(count, (uint32_t)1);

    /* Remaining segment (now at index 0) should be "BBBB" */
    free(output); free(msg);
    msg = build_shared_read_msg(0, 0, 4, &msg_len);
    output = NULL; output_len = 0;
    rc = hl_cap_wasm_call(&cache, "shared_read",
                           msg, msg_len, &output, &output_len,
                           NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, (size_t)4);
    ASSERT_EQ(memcmp(output, "BBBB", 4), 0);

    free(output); free(msg);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, shared_data_no_data)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;

    /* Call shared_read without loading any data - should return 0 */
    size_t msg_len;
    uint8_t *msg = build_shared_read_msg(0, 0, 4, &msg_len);
    void *output = NULL;
    size_t output_len = 0;

    int rc = hl_cap_wasm_call(&cache, "shared_read",
                               msg, msg_len, &output, &output_len,
                               NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, (size_t)0);  /* base == 0, returns 0 */

    free(output); free(msg);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, shared_data_segment_count)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    const char d[] = "X";

    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "s0",
              d, 1, NULL, &vfs, NULL, &err), 0);
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "s1",
              d, 1, NULL, &vfs, NULL, &err), 0);
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "s2",
              d, 1, NULL, &vfs, NULL, &err), 0);

    /* Count query */
    size_t msg_len;
    uint8_t *msg = build_shared_read_msg(0xFF, 0, 0, &msg_len);
    void *output = NULL;
    size_t output_len = 0;

    int rc = hl_cap_wasm_call(&cache, "shared_read",
                               msg, msg_len, &output, &output_len,
                               NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, (size_t)4);
    uint32_t count;
    memcpy(&count, output, 4);
    ASSERT_EQ(count, (uint32_t)3);

    free(output); free(msg);
    hl_cap_wasm_destroy(&cache);
}

/* ── Audit 5: segment chain accounting (H1) + pooled chain staleness (M1) ── */

static HlWasmModule *find_mod(HlWasmCache *cache, const char *name)
{
    return hl_cap_wasm_module_lookup(cache, name);
}

/* H1: every teardown detaches the chain, so many failed / unpooled calls leave
 * no attachment behind (WAMR's uint8 count used to grow by one per destroyed
 * instance and wrap at 256), and the segment can still be removed, re-added
 * and read afterwards. */
UTEST(hl_cap_wasm, chain_detached_on_every_teardown)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);
    const char *err = NULL;

    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
                                    "ABCDEFGHIJ", 10, NULL, &vfs, NULL, &err), 0);
    HlWasmModule *mod = find_mod(&cache, "shared_read");
    ASSERT_TRUE(mod != NULL);

    size_t msg_len;
    uint8_t *msg = build_shared_read_msg(0, 2, 5, &msg_len);

    /* 300 failed calls (gas exhaustion): each a fresh, attached instance that
     * is destroyed, never pooled. */
    HlWasmCallOpts starve = {0};
    starve.gas = 1;
    for (int i = 0; i < 300; i++) {
        void *out = NULL;
        size_t out_len = 0;
        ASSERT_EQ(hl_cap_wasm_call(&cache, "shared_read", msg, msg_len,
                                   &out, &out_len, &starve, NULL, NULL,
                                   &vfs, NULL, NULL, &err), HL_WASM_ERR_GAS);
        free(out);
    }
    /* 300 successful calls too large to pool: destroyed on release. */
    HlWasmCallOpts big = {0};
    big.heap_size = 8 * 1024 * 1024;
    for (int i = 0; i < 300; i++) {
        void *out = NULL;
        size_t out_len = 0;
        ASSERT_EQ(hl_cap_wasm_call(&cache, "shared_read", msg, msg_len,
                                   &out, &out_len, &big, NULL, NULL,
                                   &vfs, NULL, NULL, &err), 0);
        ASSERT_EQ(out_len, (size_t)5);
        free(out);
    }
    pthread_mutex_lock(&mod->mutex);
    int attached = mod->chain_attached;
    int pooled = mod->pool.count;
    pthread_mutex_unlock(&mod->mutex);
    ASSERT_EQ(attached, pooled);   /* only pooled instances hold it */

    /* Removing the segment works (the count is really 0 once drained)... */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
                                    NULL, 0, NULL, &vfs, NULL, &err), 0);
    ASSERT_EQ(mod->chain_attached, 0);
    /* ...and so do a re-add and a read through it. */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
                                    "ABCDEFGHIJ", 10, NULL, &vfs, NULL, &err), 0);
    void *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(hl_cap_wasm_call(&cache, "shared_read", msg, msg_len,
                               &out, &out_len, NULL, NULL, NULL,
                               &vfs, NULL, NULL, &err), 0);
    ASSERT_EQ(out_len, (size_t)5);
    ASSERT_EQ(memcmp(out, "CDEFG", 5), 0);
    free(out);
    free(msg);
    hl_cap_wasm_destroy(&cache);
}

/* H1: live attachments are bounded below WAMR's uint8, and a segment change
 * is refused - not half-applied - while instances outside the pool hold the
 * chain. */
UTEST(hl_cap_wasm, chain_attachments_bounded_and_change_refused_while_held)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);
    const char *err = NULL;

    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
                                    "ABCDEFGHIJ", 10, NULL, &vfs, NULL, &err), 0);
    HlWasmModule *mod = find_mod(&cache, "shared_read");
    ASSERT_TRUE(mod != NULL);

    enum { N = HL_WASM_MAX_CHAIN_ATTACH + 8 };
    static HlWasmInstance *insts[N];
    /* the live-instance cap (audit 10) is below the chain bound; lift it so
     * the chain bound is what this test reaches */
    cache.max_live_instances = N + 1;
    HlWasmCallOpts small = {0};
    small.heap_size = 64 * 1024;
    small.max_input = 64;      /* the I/O buffers must fit the small heap */
    small.max_output = 64;
    int made = 0;
    const char *last_err = NULL;
    for (int i = 0; i < N; i++) {
        insts[i] = hl_cap_wasm_instance_create(&cache, "shared_read", &small,
                                               &vfs, NULL, NULL, &last_err);
        if (!insts[i]) break;
        made++;
    }
    ASSERT_EQ(made, HL_WASM_MAX_CHAIN_ATTACH);
    ASSERT_TRUE(last_err != NULL);
    ASSERT_STREQ(last_err, "too_many_instances");
    ASSERT_EQ(mod->chain_attached, HL_WASM_MAX_CHAIN_ATTACH);

    /* Held: every kind of change is refused, and the module still works. */
    err = NULL;
    ASSERT_NE(hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
                                    NULL, 0, NULL, &vfs, NULL, &err), 0);
    ASSERT_STREQ(err, "segments_in_use");
    ASSERT_NE(hl_cap_wasm_data_load(&cache, "shared_read", "seg1",
                                    "XYZ", 3, NULL, &vfs, NULL, &err), 0);
    ASSERT_STREQ(err, "segments_in_use");
    size_t msg_len;
    uint8_t *msg = build_shared_read_msg(0, 2, 5, &msg_len);
    void *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(hl_cap_wasm_instance_call(insts[0], msg, msg_len, &out, &out_len,
                                        NULL, NULL, NULL, NULL, &err), 0);
    ASSERT_EQ(out_len, (size_t)5);
    ASSERT_EQ(memcmp(out, "CDEFG", 5), 0);
    free(out);

    for (int i = 0; i < made; i++)
        hl_cap_wasm_instance_destroy(insts[i]);
    ASSERT_EQ(mod->chain_attached, 0);
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
                                    NULL, 0, NULL, &vfs, NULL, &err), 0);
    free(msg);
    hl_cap_wasm_destroy(&cache);
}

/* M1: an instance a zero-copy result held across a segment change carries no
 * chain (it was made before the segment existed). Released, it must not go
 * back to the pool - pooled, the next call ran without the segment DATA_INFO
 * advertised. */
UTEST(hl_cap_wasm, stale_chain_instance_never_pooled)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);
    const char *err = NULL;

    size_t cnt_len;
    uint8_t *cnt = build_shared_read_msg(0xFF, 0, 0, &cnt_len);
    HlWasmBuffer *held = NULL;
    ASSERT_EQ(hl_cap_wasm_call_buf(&cache, "shared_read", cnt, cnt_len, &held,
                                   NULL, NULL, NULL, &vfs, NULL, NULL, &err), 0);
    ASSERT_TRUE(held != NULL);
    ASSERT_EQ(held->kind, HL_WASM_BUF_WASM);   /* instance checked out */

    /* The held instance has no chain attached, so the add is allowed. */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
                                    "ABCDEFGHIJ", 10, NULL, &vfs, NULL, &err), 0);
    HlWasmModule *mod = find_mod(&cache, "shared_read");
    ASSERT_TRUE(mod != NULL);

    hl_wasm_buffer_close(held);
    ASSERT_EQ(mod->pool.count, 0);   /* stale: destroyed, not pooled */

    size_t msg_len;
    uint8_t *msg = build_shared_read_msg(0, 2, 5, &msg_len);
    void *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(hl_cap_wasm_call(&cache, "shared_read", msg, msg_len,
                               &out, &out_len, NULL, NULL, NULL,
                               &vfs, NULL, NULL, &err), 0);
    ASSERT_EQ(out_len, (size_t)5);
    ASSERT_EQ(memcmp(out, "CDEFG", 5), 0);
    free(out);

    /* A persistent instance made before the segment attaches it on its next
     * call instead of reading addresses nothing backs. */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", NULL,
                                    NULL, 0, NULL, &vfs, NULL, &err), 0);
    HlWasmInstance *pi = hl_cap_wasm_instance_create(&cache, "shared_read",
                                                     NULL, &vfs, NULL, NULL, &err);
    ASSERT_TRUE(pi != NULL);
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
                                    "ABCDEFGHIJ", 10, NULL, &vfs, NULL, &err), 0);
    out = NULL;
    ASSERT_EQ(hl_cap_wasm_instance_call(pi, msg, msg_len, &out, &out_len,
                                        NULL, NULL, NULL, NULL, &err), 0);
    ASSERT_EQ(out_len, (size_t)5);
    ASSERT_EQ(memcmp(out, "CDEFG", 5), 0);
    free(out);
    hl_cap_wasm_instance_destroy(pi);

    free(msg);
    free(cnt);
    hl_cap_wasm_destroy(&cache);
}

/* ── Audit 5 M2: wall-clock watchdog ─────────────────────────────────── */

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* With gas effectively out of the way (the maximum), only the watchdog can
 * stop `(loop br 0)` - exactly the position an AOT module is always in, since
 * WAMR never meters AOT code. (An AOT build of the same loop takes the
 * patch-0007 loop-header check; that needs wamrc, absent here.) */
UTEST(hl_cap_wasm, watchdog_stops_unmetered_loop)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts opts = {0};
    opts.gas = HL_WASM_MAX_GAS;
    opts.timeout_ms = 200;
    void *out = NULL;
    size_t out_len = 0;
    const char *err = NULL;
    uint64_t t0 = mono_ms();
    int rc = hl_cap_wasm_call(&cache, "loop_forever", "x", 1, &out, &out_len,
                              &opts, NULL, NULL, &vfs, NULL, NULL, &err);
    uint64_t took = mono_ms() - t0;
    ASSERT_EQ(rc, HL_WASM_ERR_TIMEOUT);
    ASSERT_STREQ(err, "timeout");
    ASSERT_GE(took, (uint64_t)150);
    ASSERT_LT(took, (uint64_t)5000);
    free(out);

    /* Nothing stale is left behind: an ordinary call still runs. */
    out = NULL;
    ASSERT_EQ(hl_cap_wasm_call(&cache, "echo", "hello", 5, &out, &out_len,
                               NULL, NULL, NULL, &vfs, NULL, NULL, &err), 0);
    ASSERT_EQ(out_len, (size_t)5);
    free(out);

    /* A persistent instance is stopped the same way, and is usable again. */
    HlWasmInstance *pi = hl_cap_wasm_instance_create(&cache, "loop_forever",
                                                     &opts, &vfs, NULL, NULL, &err);
    ASSERT_TRUE(pi != NULL);
    for (int i = 0; i < 2; i++) {
        out = NULL;
        t0 = mono_ms();
        rc = hl_cap_wasm_instance_call(pi, "x", 1, &out, &out_len, &opts,
                                       NULL, NULL, NULL, &err);
        took = mono_ms() - t0;
        ASSERT_EQ(rc, HL_WASM_ERR_TIMEOUT);
        ASSERT_LT(took, (uint64_t)5000);
        free(out);
    }
    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
}

/* Per-call timeouts are clamped to the configured ceiling, like gas. */
UTEST(hl_cap_wasm, timeout_clamped_to_ceiling)
{
    HlWasmCallOpts o = {0};
    hl_cap_wasm_clamp_opts(&o, 0, 0, 0, 0, 0, 500);
    ASSERT_EQ(o.timeout_ms, 500u);          /* unset: the ceiling */
    o.timeout_ms = 100;
    hl_cap_wasm_clamp_opts(&o, 0, 0, 0, 0, 0, 500);
    ASSERT_EQ(o.timeout_ms, 100u);          /* below: kept */
    o.timeout_ms = 9000;
    hl_cap_wasm_clamp_opts(&o, 0, 0, 0, 0, 0, 500);
    ASSERT_EQ(o.timeout_ms, 500u);          /* above: clamped */
    ASSERT_EQ(hl_wasm_timeout_resolve(0), HL_WASM_DEFAULT_TIMEOUT_MS);
    ASSERT_EQ(hl_wasm_timeout_resolve((uint64_t)HL_WASM_MAX_TIMEOUT_MS * 10),
              HL_WASM_MAX_TIMEOUT_MS);
}

/* Round-6 L2: on a persistent instance the ceiling only lowers what a call
 * set; an unset field falls to the instance's own default, so
 * compute.instance("m", {timeout_ms = 100}) under a 60 s ceiling stops in
 * ~100 ms, not 60 s. */
UTEST(hl_cap_wasm, instance_default_timeout_under_ceiling)
{
    HlWasmCallOpts o = {0};
    hl_cap_wasm_cap_call_opts(&o, 0, 0, 0, 0, 1000, 60000);
    ASSERT_EQ(o.timeout_ms, 0u);            /* unset stays unset */
    ASSERT_EQ(o.gas, 0);
    o.timeout_ms = 90000;
    o.gas = 5000;
    hl_cap_wasm_cap_call_opts(&o, 0, 0, 0, 0, 1000, 60000);
    ASSERT_EQ(o.timeout_ms, 60000u);        /* above: clamped */
    ASSERT_EQ(o.gas, 1000);

    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);
    HlWasmCallOpts create = {0};
    create.gas = HL_WASM_MAX_GAS;
    create.timeout_ms = 100;
    hl_cap_wasm_clamp_opts(&create, 0, 0, 0, 0, 0, 60000);
    ASSERT_EQ(create.timeout_ms, 100u);
    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(&cache, "loop_forever",
                                                     &create, &vfs, NULL, NULL, &err);
    ASSERT_TRUE(pi != NULL);
    HlWasmCallOpts call = {0};
    hl_cap_wasm_cap_call_opts(&call, 0, 0, 0, 0, 0, 60000);
    void *out = NULL; size_t out_len = 0;
    uint64_t t0 = mono_ms();
    int rc = hl_cap_wasm_instance_call(pi, "x", 1, &out, &out_len, &call,
                                       NULL, NULL, NULL, &err);
    uint64_t took = mono_ms() - t0;
    EXPECT_EQ(rc, HL_WASM_ERR_TIMEOUT);
    EXPECT_LT(took, (uint64_t)5000);
    free(out);
    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
}

/* A start function runs inside instantiation, where gas never applied: the
 * watch armed for the call is bound to the new instance while it runs. */
UTEST(hl_cap_wasm, watchdog_stops_start_function)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    char ebuf[256];
    uint8_t *copy = malloc(start_loop_wasm_len);
    ASSERT_TRUE(copy != NULL);
    memcpy(copy, start_loop_wasm, start_loop_wasm_len);
    wasm_module_t m = wasm_runtime_load(copy, start_loop_wasm_len,
                                        ebuf, sizeof ebuf);
    ASSERT_TRUE(m != NULL);

    HlWasmWatch w = {0};
    ASSERT_EQ(hl_wasm_watch_arm(&w, 200, NULL), 0);
    uint64_t t0 = mono_ms();
    ebuf[0] = '\0';
    void *inst = hl_wasm_watch_instantiate(&w, m, 8192, 8192, ebuf, sizeof ebuf);
    uint64_t took = mono_ms() - t0;
    ASSERT_EQ(hl_wasm_watch_disarm(&w), 1);
    ASSERT_TRUE(inst == NULL);
    ASSERT_TRUE(strstr(ebuf, "terminated") != NULL);
    ASSERT_LT(took, (uint64_t)5000);

    wasm_runtime_unload(m);
    free(copy);
    hl_cap_wasm_destroy(&cache);
}

/* ...and through the module loader: the load-time probe instantiation is
 * bounded by the DEFAULT timeout, and a module whose start function outlives
 * it is refused rather than cached to hang every later call. */
UTEST(hl_cap_wasm, load_refuses_module_whose_start_never_returns)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    uint64_t t0 = mono_ms();
    int rc = hl_cap_wasm_load(&cache, "start_loop", &vfs, NULL);
    uint64_t took = mono_ms() - t0;
    ASSERT_EQ(rc, HL_WASM_ERR_LOAD);
    ASSERT_EQ(cache.count, 0);
    ASSERT_LT(took, (uint64_t)HL_WASM_DEFAULT_TIMEOUT_MS + 5000);
    hl_cap_wasm_destroy(&cache);
}

/* A module that exports its own malloc / free is refused at load (audit 23):
 * WAMR would insert no app heap and run the guest's malloc for every buffer
 * Hull allocates, trusting the offset it returns. WAMR itself loads the
 * module - the refusal is Hull's. */
UTEST(hl_cap_wasm, load_refuses_module_exporting_allocator)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    char ebuf[256];
    uint8_t *copy = malloc(guest_alloc_wasm_len);
    ASSERT_TRUE(copy != NULL);
    memcpy(copy, guest_alloc_wasm, guest_alloc_wasm_len);
    wasm_module_t m = wasm_runtime_load(copy, guest_alloc_wasm_len,
                                        ebuf, sizeof ebuf);
    ASSERT_TRUE(m != NULL);
    wasm_runtime_unload(m);
    free(copy);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);
    ASSERT_EQ(hl_cap_wasm_load(&cache, "guest_alloc", &vfs, NULL),
              HL_WASM_ERR_LOAD);
    ASSERT_EQ(cache.count, 0);

    /* A call loads on demand: it fails without running the guest. */
    HlWasmBuffer *out = NULL;
    const char *err = NULL;
    ASSERT_NE(hl_cap_wasm_call_buf(&cache, "guest_alloc", "abc", 3, &out,
                                   NULL, NULL, NULL, &vfs, NULL, NULL, &err),
              HL_WASM_OK);
    ASSERT_TRUE(out == NULL);

    /* An ordinary module still loads. */
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, shared_data_concurrent_calls)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *err = NULL;
    const char data[] = "CONCURRENT_TEST_DATA";  /* 20 bytes */

    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "shared_read", "seg0",
              data, 20, NULL, &vfs, NULL, &err), 0);

    /* Multiple sequential calls from same cache - simulates pool usage */
    for (int i = 0; i < 10; i++) {
        size_t msg_len;
        uint8_t *msg = build_shared_read_msg(0, 0, 20, &msg_len);
        void *output = NULL;
        size_t output_len = 0;

        int rc = hl_cap_wasm_call(&cache, "shared_read",
                                   msg, msg_len, &output, &output_len,
                                   NULL, NULL, NULL, &vfs, NULL, NULL, &err);
        ASSERT_EQ(rc, 0);
        ASSERT_EQ(output_len, (size_t)20);
        ASSERT_EQ(memcmp(output, data, 20), 0);

        free(output); free(msg);
    }

    hl_cap_wasm_destroy(&cache);
}

/* ── Memory64 tests ────────────────────────────────────────────────── */

UTEST(hl_cap_wasm, memory64_detection)
{
    /* echo64.wasm has Memory64 flag. Load it and verify detection. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* Detection runs through the public accessor wasm_runtime_memory_is_memory64
     * (patch 0005), independent of any WAMR compile-time macro in this TU, so the
     * assertion is unconditional (was #if WASM_ENABLE_MEMORY64 gated / #else no-op
     * before #318). echo64 loads as interpreter (no .aot embedded); the Memory64
     * flag is still detected at load. */
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo64", &vfs, NULL), 0);

    pthread_mutex_lock(&cache.pool_mutex);
    HlWasmModule *mod = NULL;
    for (int i = 0; i < cache.count; i++) {
        if (strcmp(cache.modules[i].name, "echo64") == 0) {
            mod = &cache.modules[i];
            break;
        }
    }
    pthread_mutex_unlock(&cache.pool_mutex);

    ASSERT_NE(mod, NULL);
    ASSERT_EQ(mod->is_memory64, 1);
    ASSERT_EQ(mod->is_aot, 0);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, memory64_rejects_interpreter)
{
    /* Non-AOT Memory64 module should be rejected at call time */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* A non-AOT Memory64 module is rejected at call time by Hull's own guard
     * (cap/wasm.c: is_memory64 && !is_aot), now that detection is live (#318). */
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo64",
                               "test", 4, &output, &output_len,
                               NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_NE(rc, 0);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "memory64_requires_aot");
    free(output);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, wasm32_not_memory64)
{
    /* Regular echo.wasm should NOT be detected as Memory64 */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);

    pthread_mutex_lock(&cache.pool_mutex);
    HlWasmModule *mod = NULL;
    for (int i = 0; i < cache.count; i++) {
        if (strcmp(cache.modules[i].name, "echo") == 0) {
            mod = &cache.modules[i];
            break;
        }
    }
    pthread_mutex_unlock(&cache.pool_mutex);

    ASSERT_NE(mod, NULL);
    ASSERT_EQ(mod->is_memory64, 0);

    hl_cap_wasm_destroy(&cache);
}

/* #318 D4.3: the crux -- a real Memory64 AOT module run through hl_cap_wasm_call,
 * exercising the 8-cell hull_process(i64,i64,i64,i64) argv marshalling and the
 * output readback (the path that was dead while detection was compiled out).
 * echo64 AOT-compiled by wamrc (which auto-detects Memory64 from the module --
 * there is no --enable-memory64 flag) loads as AOT + Memory64; the call
 * echoes its input. Skips when wamrc is unavailable (e.g. an LLVM too new for the
 * vendored WAMR compiler); the memory64-aot CI job builds wamrc and asserts this
 * case is NOT skipped. */
UTEST(hl_cap_wasm, memory64_aot_dispatch)
{
    if (echo64_aot_len == 0)
        UTEST_SKIP("no wamrc-built echo64 .aot in this build leg");

    /* The VFS AOT key must match hl_cap_wasm_load's compute/<name>.aot.<arch>. */
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    static const char *aot_key = "compute/echo64.aot.x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    static const char *aot_key = "compute/echo64.aot.aarch64";
#else
    UTEST_SKIP("unsupported arch for the Memory64 AOT dispatch test");
#endif

    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlEntry entries[] = { { aot_key, echo64_aot, echo64_aot_len } };
    HlVfs vfs;
    hl_vfs_init(&vfs, entries, NULL);

    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo64", &vfs, NULL), 0);

    pthread_mutex_lock(&cache.pool_mutex);
    HlWasmModule *mod = NULL;
    for (int i = 0; i < cache.count; i++) {
        if (strcmp(cache.modules[i].name, "echo64") == 0) {
            mod = &cache.modules[i];
            break;
        }
    }
    pthread_mutex_unlock(&cache.pool_mutex);
    ASSERT_NE(mod, NULL);
    ASSERT_EQ(mod->is_memory64, 1);   /* detected */
    ASSERT_EQ(mod->is_aot, 1);        /* loaded as AOT (so the mem64 guard passes) */

    /* 8-cell dispatch + readback: echo64 copies input -> output. */
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;
    int rc = hl_cap_wasm_call(&cache, "echo64", "hello64", 7, &output, &output_len,
                              NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(output_len, (size_t)7);
    ASSERT_NE(output, NULL);
    ASSERT_EQ(memcmp(output, "hello64", 7), 0);
    free(output);

    hl_cap_wasm_destroy(&cache);
}

/* ── Streaming I/O tests ──────────────────────────────────────────── */

UTEST(hl_cap_wasm, stream_echo_basic)
{
    /* Stream 1000 bytes through echo with chunk_size=256 (min), verify output == input */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* Build 1000-byte input */
    char input[1000];
    for (int i = 0; i < 1000; i++) input[i] = (char)(i & 0xFF);

    HlStreamInput in = {0};
    in.kind = HL_STREAM_IN_BUFFER;
    in.buffer.data = input;
    in.buffer.len = 1000;

    void *out_data = NULL;
    size_t out_len = 0;
    HlStreamOutput out = {0};
    out.kind = HL_STREAM_OUT_BUFFER;
    out.buffer.data = &out_data;
    out.buffer.len = &out_len;

    HlStreamOpts opts = {0};
    opts.chunk_size = 256; /* min chunk size */

    HlStreamResult res = {0};
    const char *err = NULL;

    int rc = hl_cap_wasm_stream(&cache, "echo", &in, &out, &opts,
                                 NULL, &vfs, NULL, NULL, &res, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(out_len, (size_t)1000);
    ASSERT_EQ(memcmp(out_data, input, 1000), 0);
    ASSERT_EQ(res.total_input, (size_t)1000);
    ASSERT_EQ(res.total_output, (size_t)1000);
    /* 1000 / 256 = 3.90625 → 4 chunks */
    ASSERT_EQ(res.chunks, (uint32_t)4);

    free(out_data);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, stream_echo_single_chunk)
{
    /* Input smaller than chunk → single chunk with first+last flags */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *input = "hello";

    HlStreamInput in = {0};
    in.kind = HL_STREAM_IN_BUFFER;
    in.buffer.data = input;
    in.buffer.len = 5;

    void *out_data = NULL;
    size_t out_len = 0;
    HlStreamOutput out = {0};
    out.kind = HL_STREAM_OUT_BUFFER;
    out.buffer.data = &out_data;
    out.buffer.len = &out_len;

    HlStreamOpts opts = {0};
    opts.chunk_size = 4096; /* much larger than input */

    HlStreamResult res = {0};
    const char *err = NULL;

    int rc = hl_cap_wasm_stream(&cache, "echo", &in, &out, &opts,
                                 NULL, &vfs, NULL, NULL, &res, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(out_len, (size_t)5);
    ASSERT_EQ(memcmp(out_data, "hello", 5), 0);
    ASSERT_EQ(res.chunks, (uint32_t)1);

    free(out_data);
    hl_cap_wasm_destroy(&cache);
}

/* Audit 15: the per-chunk hook runs before every chunk and stops the stream
 * when it says so - the bindings charge the run's budget and check its
 * deadline there; before, a stream of many chunks ran them all inside one
 * binding call. And buffer-mode output is capped (max_output). */
typedef struct { int calls; int stop_at; size_t bytes; } StreamHookState;

static int stream_test_hook(size_t chunk_len, void *ud)
{
    StreamHookState *s = (StreamHookState *)ud;
    s->calls++;
    s->bytes += chunk_len;
    return s->calls > s->stop_at;
}

UTEST(hl_cap_wasm, stream_before_chunk_hook_stops_the_stream)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    static char input[256 * 40];
    memset(input, 'q', sizeof input);
    HlStreamInput in = {0};
    in.kind = HL_STREAM_IN_BUFFER;
    in.buffer.data = input;
    in.buffer.len = sizeof input;

    void *out_data = NULL;
    size_t out_len = 0;
    HlStreamOutput out = {0};
    out.kind = HL_STREAM_OUT_BUFFER;
    out.buffer.data = &out_data;
    out.buffer.len = &out_len;

    /* A hook that never stops sees every chunk, with its length. */
    StreamHookState st = { 0, 1000, 0 };
    HlStreamOpts opts = {0};
    opts.chunk_size = 256;
    opts.before_chunk = stream_test_hook;
    opts.before_chunk_ud = &st;
    HlStreamResult res = {0};
    const char *err = NULL;
    ASSERT_EQ(hl_cap_wasm_stream(&cache, "echo", &in, &out, &opts,
                                 NULL, &vfs, NULL, NULL, &res, &err),
              HL_WASM_OK);
    EXPECT_EQ(st.calls, 40);
    EXPECT_EQ(st.bytes, sizeof input);
    EXPECT_EQ(res.chunks, (uint32_t)40);
    free(out_data);
    out_data = NULL;

    /* One that stops after 5: chunk 6 never runs, no output is handed back. */
    st = (StreamHookState){ 0, 5, 0 };
    memset(&res, 0, sizeof res);
    err = NULL;
    EXPECT_NE(hl_cap_wasm_stream(&cache, "echo", &in, &out, &opts,
                                 NULL, &vfs, NULL, NULL, &res, &err),
              HL_WASM_OK);
    EXPECT_TRUE(err && strcmp(err, "stopped") == 0);
    EXPECT_EQ(st.calls, 6);
    EXPECT_EQ(res.chunks, (uint32_t)5);
    EXPECT_EQ(out_data, NULL);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, stream_buffer_output_is_capped)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    static char input[4096];
    memset(input, 'z', sizeof input);
    HlStreamInput in = {0};
    in.kind = HL_STREAM_IN_BUFFER;
    in.buffer.data = input;
    in.buffer.len = sizeof input;

    void *out_data = NULL;
    size_t out_len = 0;
    HlStreamOutput out = {0};
    out.kind = HL_STREAM_OUT_BUFFER;
    out.buffer.data = &out_data;
    out.buffer.len = &out_len;

    HlStreamOpts opts = {0};
    opts.chunk_size = 256;
    opts.max_output = 1000;     /* the echo's 4096 bytes do not fit */
    HlStreamResult res = {0};
    const char *err = NULL;
    EXPECT_NE(hl_cap_wasm_stream(&cache, "echo", &in, &out, &opts,
                                 NULL, &vfs, NULL, NULL, &res, &err),
              HL_WASM_OK);
    EXPECT_TRUE(err && strcmp(err, "output_too_large") == 0);
    EXPECT_EQ(out_data, NULL);

    /* Exactly at the cap is fine. */
    opts.max_output = sizeof input;
    err = NULL;
    ASSERT_EQ(hl_cap_wasm_stream(&cache, "echo", &in, &out, &opts,
                                 NULL, &vfs, NULL, NULL, &res, &err),
              HL_WASM_OK);
    EXPECT_EQ(out_len, sizeof input);
    free(out_data);

    hl_cap_wasm_destroy(&cache);
}

/* Callback test state */
static int cb_test_call_count;
static int cb_test_last_is_last;
static size_t cb_test_total_bytes;

static int stream_test_callback(const void *data, size_t len,
                                 uint32_t index, int is_last,
                                 void *user_data)
{
    (void)data; (void)user_data;
    cb_test_call_count++;
    cb_test_last_is_last = is_last;
    cb_test_total_bytes += len;
    /* Verify chunk index matches call count (0-indexed) */
    if (index != (uint32_t)(cb_test_call_count - 1)) return -1;
    return 0;
}

UTEST(hl_cap_wasm, stream_callback)
{
    /* Use callback output, verify called per chunk with correct index/is_last */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* 768 bytes / 256 (min chunk) = 3 chunks exactly */
    char input[768];
    memset(input, 'A', 768);

    cb_test_call_count = 0;
    cb_test_last_is_last = 0;
    cb_test_total_bytes = 0;

    HlStreamInput in = {0};
    in.kind = HL_STREAM_IN_BUFFER;
    in.buffer.data = input;
    in.buffer.len = 768;

    HlStreamOutput out = {0};
    out.kind = HL_STREAM_OUT_CALLBACK;
    out.callback.fn = stream_test_callback;
    out.callback.user_data = NULL;

    HlStreamOpts opts = {0};
    opts.chunk_size = 256; /* min chunk size */

    HlStreamResult res = {0};
    const char *err = NULL;

    int rc = hl_cap_wasm_stream(&cache, "echo", &in, &out, &opts,
                                 NULL, &vfs, NULL, NULL, &res, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(cb_test_call_count, 3);
    ASSERT_EQ(cb_test_last_is_last, 1);
    ASSERT_EQ(cb_test_total_bytes, (size_t)768);
    ASSERT_EQ(res.chunks, (uint32_t)3);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, stream_empty_input)
{
    /* Zero-length input: 1 chunk processed with first+last flags */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlStreamInput in = {0};
    in.kind = HL_STREAM_IN_BUFFER;
    in.buffer.data = NULL;
    in.buffer.len = 0;

    void *out_data = NULL;
    size_t out_len = 0;
    HlStreamOutput out = {0};
    out.kind = HL_STREAM_OUT_BUFFER;
    out.buffer.data = &out_data;
    out.buffer.len = &out_len;

    HlStreamResult res = {0};
    const char *err = NULL;

    int rc = hl_cap_wasm_stream(&cache, "echo", &in, &out, NULL,
                                 NULL, &vfs, NULL, NULL, &res, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(out_len, (size_t)0);
    ASSERT_EQ(res.total_input, (size_t)0);
    ASSERT_EQ(res.total_output, (size_t)0);
    ASSERT_EQ(res.chunks, (uint32_t)1);

    free(out_data);
    hl_cap_wasm_destroy(&cache);
}

/* Audit 9 M1: a BUFFER-mode result is handed over at exactly the length
 * reported, so the caller's hl_alloc_free(alloc, data, len) - what both
 * runtimes do - returns the allocator to where it was. The buffer used to
 * keep its grown capacity (1024 for 1000 bytes here), and an empty result
 * handed over the whole first chunk with len 0, charged for good. */
static int stream_alloc_roundtrip(const char *input, size_t input_len,
                                  size_t expect_len)
{
    int bad = 0;
    HlWasmCache cache;
    if (hl_cap_wasm_init(&cache) != 0) return 1;
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlStreamInput in = {0};
    in.kind = HL_STREAM_IN_BUFFER;
    in.buffer.data = input;
    in.buffer.len = input_len;
    HlStreamOpts opts = {0};
    opts.chunk_size = 256;

    HlAllocator alloc;
    hl_alloc_init(&alloc, 0);
    for (int round = 0; round < 2; round++) {
        void *out_data = NULL;
        size_t out_len = 0;
        HlStreamOutput out = {0};
        out.kind = HL_STREAM_OUT_BUFFER;
        out.buffer.data = &out_data;
        out.buffer.len = &out_len;
        HlStreamResult res = {0};
        const char *err = NULL;
        size_t before = atomic_load(&alloc.used);
        int rc = hl_cap_wasm_stream(&cache, "echo", &in, &out, &opts,
                                    NULL, &vfs, NULL, &alloc, &res, &err);
        if (rc != HL_WASM_OK || out_len != expect_len ||
            (expect_len && memcmp(out_data, input, expect_len) != 0)) {
            bad = 1;
            free(out_data);
            break;
        }
        if (expect_len == 0 && out_data != NULL) bad = 1;
        hl_alloc_free(&alloc, out_data, out_len);
        /* Round 0 may warm the module cache; round 1 must net to zero. */
        if (round == 1 && atomic_load(&alloc.used) != before)
            bad = 1;
    }
    hl_cap_wasm_destroy(&cache);
    return bad;
}

UTEST(hl_cap_wasm, stream_buffer_handover_exact_len)
{
    char input[1000];
    for (int i = 0; i < 1000; i++) input[i] = (char)(i & 0xFF);
    ASSERT_EQ(stream_alloc_roundtrip(input, sizeof input, sizeof input), 0);
}

UTEST(hl_cap_wasm, stream_buffer_handover_empty_output)
{
    ASSERT_EQ(stream_alloc_roundtrip(NULL, 0, 0), 0);
}

UTEST(hl_cap_wasm, stream_gas_exhaustion)
{
    /* Very low gas should cause error on chunk processing */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    char input[256];
    memset(input, 'X', 256);

    HlStreamInput in = {0};
    in.kind = HL_STREAM_IN_BUFFER;
    in.buffer.data = input;
    in.buffer.len = 256;

    void *out_data = NULL;
    size_t out_len = 0;
    HlStreamOutput out = {0};
    out.kind = HL_STREAM_OUT_BUFFER;
    out.buffer.data = &out_data;
    out.buffer.len = &out_len;

    HlStreamOpts opts = {0};
    opts.chunk_size = 64;
    opts.call_opts.gas = 1; /* impossibly low gas */

    const char *err = NULL;
    HlStreamResult res = {0};

    int rc = hl_cap_wasm_stream(&cache, "echo", &in, &out, &opts,
                                 NULL, &vfs, NULL, NULL, &res, &err);
    ASSERT_NE(rc, HL_WASM_OK);

    free(out_data);
    hl_cap_wasm_destroy(&cache);
}

/* ── Sealed native-symbol table (audit H1) ────────────────────────── */
/*
 * Fork+SIGSEGV death test: after hl_cap_wasm_init returns, the
 * NativeSymbol table WAMR was registered with lives in a sealed
 * (mprotect-RO) arena.  Any write to it must fault.  Without the
 * H1 fix the table was a `static` writable global and a heap-write
 * primitive could rewrite the host_call dispatcher pointer to pivot
 * control flow on the next WASM→host call.
 *
 * The child resets SIGSEGV/SIGBUS to SIG_DFL before writing so the
 * signal propagates as termination (WIFSIGNALED).  Without the
 * reset, ASan/MSan's own SEGV handler intercepts, prints a
 * diagnostic, and _exit(1)s - making WIFSIGNALED false and the test
 * spuriously fail.  Same pattern as test_seal_arena.c and
 * test_manifest_seal.c.
 */
#include <sh_seal_arena.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

UTEST(hl_cap_wasm, host_symbols_table_sealed_after_init)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    /* The host_call native is registered.  Its NativeSymbol slot
     * lives in cache.native_arena which is now sealed RO.  We can't
     * easily get the slot's address from outside without poking
     * WAMR internals - so instead we write to the arena's base
     * directly; if the seal worked, ANY page in the arena will
     * fault on write. */
    ASSERT_TRUE(cache.native_arena != NULL);
    ASSERT_TRUE(cache.native_arena->base != NULL);
    ASSERT_EQ(cache.native_arena->sealed, 1);

    pid_t pid = fork();
    if (pid == 0) {
        /* Reset sanitizer-installed handlers so the signal terminates
         * the child instead of being intercepted. */
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS,  SIG_DFL);
        ((volatile char *)cache.native_arena->base)[0] = 'X';
        _exit(0);
    }
    ASSERT_TRUE(pid > 0);

    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    ASSERT_EQ(pid, w);
    ASSERT_TRUE(WIFSIGNALED(status));
    int sig = WTERMSIG(status);
    ASSERT_TRUE(sig == SIGSEGV || sig == SIGBUS);

    hl_cap_wasm_destroy(&cache);
}

#else /* !HL_ENABLE_WASM */

UTEST(hl_cap_wasm, disabled_placeholder)
{
    /* WASM support not compiled - test passes as no-op */
    ASSERT_TRUE(1);
}

#endif /* HL_ENABLE_WASM */

/* Serialize log.c before any concurrent test (concurrent_load, pool_stress,
 * the snapshot-hold test) - vendored log.c is not thread-safe without it. */
UTEST_STATE();
/* ── Round 5: the compute loader's disk fallbacks (H2) ─────────────── */

#include "hull/cap/crypto.h"
#include "../test_tmpdir.h"
#include <sys/stat.h>

static int wr_file(const char *path, const void *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(d, 1, n, f);
    return (fclose(f) == 0 && w == n) ? 0 : -1;
}

static int mk_compute_app(char *dir, size_t n)
{
    if (!hl_test_mkdtemp(dir, n, "hull_wasm_disk")) return -1;
    char p[600];
    snprintf(p, sizeof p, "%s/compute", dir);
    if (mkdir(p, 0755) != 0) return -1;
    snprintf(p, sizeof p, "%s/compute/echo.wasm", dir);
    return wr_file(p, echo_wasm, echo_wasm_len);
}

/* A built binary (app files embedded) runs the modules it was built with:
 * it used to fall back to <cwd>/compute/<name>.aot.<arch> - native code
 * outside the WASM sandbox and outside any signature - for a name it did
 * not embed. */
UTEST(hl_cap_wasm, built_binary_does_not_load_modules_from_disk)
{
    char dir[512];
    ASSERT_EQ(mk_compute_app(dir, sizeof dir), 0);
    static const HlEntry other[] = {
        { "compute/echo64.wasm", echo64_wasm, echo64_wasm_len },
        { 0, 0, 0 }
    };
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, other, dir);
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, dir), HL_WASM_ERR_NOT_FOUND);
    hl_cap_wasm_destroy(&cache);

    /* In development (nothing embedded) the same file loads. */
    static const HlEntry none[] = { { 0, 0, 0 } };
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    hl_vfs_init(&vfs, none, dir);
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, dir), 0);
    hl_cap_wasm_destroy(&cache);
}

/* Audit 24: the dev fallback opened <app_dir>/compute/<name>.wasm with a
 * plain fopen - a symlink was followed out of the app tree and a FIFO
 * blocked the loop. It opens under the app root now: neither loads. */
UTEST(hl_cap_wasm, dev_disk_module_is_opened_under_the_app_root)
{
    char dir[512], outside[512];
    ASSERT_EQ(mk_compute_app(dir, sizeof dir), 0);
    ASSERT_EQ(mk_compute_app(outside, sizeof outside), 0);
    static const HlEntry none[] = { { 0, 0, 0 } };
    HlVfs vfs;
    hl_vfs_init(&vfs, none, dir);
    char p[700], target[700];
    snprintf(p, sizeof p, "%s/compute/echo.wasm", dir);
    snprintf(target, sizeof target, "%s/compute/echo.wasm", outside);

    ASSERT_EQ(unlink(p), 0);
    ASSERT_EQ(symlink(target, p), 0);
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, dir), HL_WASM_ERR_NOT_FOUND);
    hl_cap_wasm_destroy(&cache);

    ASSERT_EQ(unlink(p), 0);
#if !defined(__COSMOPOLITAN__)   /* cosmo does not declare mkfifo under -std=c11 */
    ASSERT_EQ(mkfifo(p, 0600), 0);   /* no writer: a blocking open hangs */
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, dir), HL_WASM_ERR_NOT_FOUND);
    hl_cap_wasm_destroy(&cache);
    unlink(p);
#endif
}

/* Under --verify-sig (the disk gate armed) only a signed module, with its
 * signed bytes, loads from disk; a planted AOT artifact is never read (no AOT
 * is loaded from disk, audit 19), so the signed .wasm runs. */
UTEST(hl_cap_wasm, verify_sig_refuses_unsigned_disk_modules)
{
    char dir[512];
    ASSERT_EQ(mk_compute_app(dir, sizeof dir), 0);
    static const HlEntry none[] = { { 0, 0, 0 } };
    HlVfs vfs;
    hl_vfs_init(&vfs, none, dir);

    const char *names[1] = { "compute/echo.wasm" };
    uint8_t dg[1][32];
    ASSERT_EQ(hl_cap_crypto_sha256(echo_wasm, echo_wasm_len, dg[0]), 0);
    ASSERT_EQ(hl_vfs_disk_gate_arm(names, (const uint8_t (*)[32])dg, 1,
                                   hl_cap_crypto_sha256), 0);

    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, dir), 0);   /* signed */
    hl_cap_wasm_destroy(&cache);

    /* A planted AOT artifact beside it. */
    const char *archs[] = { "x86_64", "aarch64" };
    for (int k = 0; k < 2; k++) {
        char p[700];
        snprintf(p, sizeof p, "%s/compute/echo.aot.%s", dir, archs[k]);
        ASSERT_EQ(wr_file(p, "NOT-SIGNED-NATIVE", 17), 0);
    }
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, dir), 0);
    pthread_mutex_lock(&cache.pool_mutex);
    int planted_aot = cache.count == 1 ? cache.modules[0].is_aot : -1;
    pthread_mutex_unlock(&cache.pool_mutex);
    EXPECT_EQ(planted_aot, 0);
    hl_cap_wasm_destroy(&cache);
    for (int k = 0; k < 2; k++) {
        char p[700];
        snprintf(p, sizeof p, "%s/compute/echo.aot.%s", dir, archs[k]);
        unlink(p);
    }

    /* A module whose bytes differ from the signed ones. */
    char p[700];
    snprintf(p, sizeof p, "%s/compute/echo.wasm", dir);
    ASSERT_EQ(wr_file(p, echo64_wasm, echo64_wasm_len), 0);
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, dir), HL_WASM_ERR_LOAD);
    hl_cap_wasm_destroy(&cache);

    hl_vfs_disk_gate_reset();
}

/* ── Round-6 M2: AOT provenance stamp ─────────────────────────────────── */

/* A minimal AOT file head: magic, version, target-info section (type 0, size
 * 48), `reserved` = @p stamp_version | @p stamp_magic << 32, rest zero. */
static void mk_aot_head(uint8_t *b, size_t n, uint32_t stamp_version,
                        uint32_t stamp_magic)
{
    memset(b, 0, n);
    const uint32_t w[] = { HL_AOT_FILE_MAGIC, 7u, 0u, HL_AOT_TARGET_INFO_SIZE };
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++)
            b[i * 4 + k] = (uint8_t)(w[i] >> (8 * k));
    for (int k = 0; k < 4; k++) {
        b[HL_AOT_STAMP_OFFSET + k]     = (uint8_t)(stamp_version >> (8 * k));
        b[HL_AOT_STAMP_OFFSET + 4 + k] = (uint8_t)(stamp_magic >> (8 * k));
    }
}

UTEST(hl_cap_wasm, aot_stamp_check)
{
    uint8_t b[96];
    mk_aot_head(b, sizeof b, HL_AOT_STAMP_LOW_OK, HL_AOT_STAMP_MAGIC);
    EXPECT_EQ(hl_aot_stamp_check(b, sizeof b), HL_AOT_STAMP_OK);
    EXPECT_EQ(hl_aot_stamp_check(b, 63), HL_AOT_STAMP_NOT_AOT);     /* short */
    EXPECT_EQ(hl_aot_stamp_check(NULL, 0), HL_AOT_STAMP_NOT_AOT);

    mk_aot_head(b, sizeof b, 0, 0);                  /* upstream wamrc: zero */
    EXPECT_EQ(hl_aot_stamp_check(b, sizeof b), HL_AOT_STAMP_MISSING);
    mk_aot_head(b, sizeof b, HL_AOT_STAMP_VERSION + 1, HL_AOT_STAMP_MAGIC);
    EXPECT_EQ(hl_aot_stamp_check(b, sizeof b), HL_AOT_STAMP_OLD_VERSION);

    /* Audit 31: the stamp must record software bounds checks. A version-1
     * stamp (a wamrc from before the flag bits) is stale; a current one
     * without both bits - wamrc's 64-bit default, guard-page code Hull's
     * runtime has no guard region for - is refused. */
    mk_aot_head(b, sizeof b, 1u, HL_AOT_STAMP_MAGIC);
    EXPECT_EQ(hl_aot_stamp_check(b, sizeof b), HL_AOT_STAMP_OLD_VERSION);
    mk_aot_head(b, sizeof b, 1u | HL_AOT_STAMP_REQUIRED, HL_AOT_STAMP_MAGIC);
    EXPECT_EQ(hl_aot_stamp_check(b, sizeof b), HL_AOT_STAMP_OLD_VERSION);
    mk_aot_head(b, sizeof b, HL_AOT_STAMP_VERSION, HL_AOT_STAMP_MAGIC);
    EXPECT_EQ(hl_aot_stamp_check(b, sizeof b), HL_AOT_STAMP_NO_BOUNDS);
    mk_aot_head(b, sizeof b, HL_AOT_STAMP_VERSION | HL_AOT_STAMP_F_BOUNDS,
                HL_AOT_STAMP_MAGIC);
    EXPECT_EQ(hl_aot_stamp_check(b, sizeof b), HL_AOT_STAMP_NO_BOUNDS);
    mk_aot_head(b, sizeof b, HL_AOT_STAMP_VERSION | HL_AOT_STAMP_F_STACK_BOUNDS,
                HL_AOT_STAMP_MAGIC);
    EXPECT_EQ(hl_aot_stamp_check(b, sizeof b), HL_AOT_STAMP_NO_BOUNDS);
    mk_aot_head(b, sizeof b, HL_AOT_STAMP_LOW_OK | 0x40000u, HL_AOT_STAMP_MAGIC);
    EXPECT_EQ(hl_aot_stamp_check(b, sizeof b), HL_AOT_STAMP_OK);

    mk_aot_head(b, sizeof b, HL_AOT_STAMP_LOW_OK, HL_AOT_STAMP_MAGIC);
    b[0] = 0x01;                                      /* not "\0aot" */
    EXPECT_EQ(hl_aot_stamp_check(b, sizeof b), HL_AOT_STAMP_NOT_AOT);
    mk_aot_head(b, sizeof b, HL_AOT_STAMP_LOW_OK, HL_AOT_STAMP_MAGIC);
    b[8] = 3;                                         /* first section not target info */
    EXPECT_EQ(hl_aot_stamp_check(b, sizeof b), HL_AOT_STAMP_NOT_AOT);

    /* A plain .wasm is not an AOT file. */
    EXPECT_EQ(hl_aot_stamp_check(echo_wasm, echo_wasm_len), HL_AOT_STAMP_NOT_AOT);
}

/* Audit 31: an AOT artifact stamped by Hull's wamrc but compiled without
 * software bounds checks is refused like an unstamped one - the .wasm runs in
 * the interpreter, and with no .wasm the load fails. */
UTEST(hl_cap_wasm, aot_without_bounds_checks_refused)
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    static const char *aot_key = "compute/echo.aot.x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    static const char *aot_key = "compute/echo.aot.aarch64";
#else
    UTEST_SKIP("no AOT arch");
#endif
    static uint8_t nobounds[128];
    mk_aot_head(nobounds, sizeof nobounds,
                HL_AOT_STAMP_VERSION | HL_AOT_STAMP_F_STACK_BOUNDS,
                HL_AOT_STAMP_MAGIC);
    ASSERT_EQ(hl_aot_stamp_check(nobounds, sizeof nobounds),
              HL_AOT_STAMP_NO_BOUNDS);

    HlEntry with_wasm[] = {
        { aot_key, nobounds, sizeof nobounds },
        { "compute/echo.wasm", echo_wasm, echo_wasm_len },
        { 0, 0, 0 }
    };
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, with_wasm, NULL);
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);
    pthread_mutex_lock(&cache.pool_mutex);
    int is_aot = cache.count == 1 ? cache.modules[0].is_aot : -1;
    pthread_mutex_unlock(&cache.pool_mutex);
    EXPECT_EQ(is_aot, 0);
    hl_cap_wasm_destroy(&cache);

    HlEntry aot_only[] = { { aot_key, nobounds, sizeof nobounds }, { 0, 0, 0 } };
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    hl_vfs_init(&vfs, aot_only, NULL);
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), HL_WASM_ERR_LOAD);
    hl_cap_wasm_destroy(&cache);

    /* The same bytes under the .wasm name (AOT magic): refused too. */
    HlEntry as_wasm[] = { { "compute/echo.wasm", nobounds, sizeof nobounds },
                          { 0, 0, 0 } };
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    hl_vfs_init(&vfs, as_wasm, NULL);
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), HL_WASM_ERR_LOAD);
    hl_cap_wasm_destroy(&cache);
}

/* An AOT artifact without the stamp (an unpatched wamrc's) is refused: the
 * module's .wasm runs in the interpreter instead, and with no .wasm the load
 * fails rather than run code the compute timeout cannot stop. */
UTEST(hl_cap_wasm, unstamped_aot_refused_with_wasm_fallback)
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    static const char *aot_key = "compute/echo.aot.x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    static const char *aot_key = "compute/echo.aot.aarch64";
#else
    UTEST_SKIP("no AOT arch");
#endif
    static uint8_t unstamped[128];
    mk_aot_head(unstamped, sizeof unstamped, 0, 0);

    /* With the .wasm beside it: loads, in the interpreter, and runs. */
    HlEntry with_wasm[] = {
        { aot_key, unstamped, sizeof unstamped },
        { "compute/echo.wasm", echo_wasm, echo_wasm_len },
        { 0, 0, 0 }
    };
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, with_wasm, NULL);
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);
    pthread_mutex_lock(&cache.pool_mutex);
    int is_aot = cache.count == 1 ? cache.modules[0].is_aot : -1;
    pthread_mutex_unlock(&cache.pool_mutex);
    EXPECT_EQ(is_aot, 0);
    void *out = NULL; size_t out_len = 0; const char *err = NULL;
    EXPECT_EQ(hl_cap_wasm_call(&cache, "echo", "hi", 2, &out, &out_len,
                               NULL, NULL, NULL, &vfs, NULL, NULL, &err), 0);
    EXPECT_EQ(out_len, (size_t)2);
    free(out);
    hl_cap_wasm_destroy(&cache);

    /* AOT only: refused, not loaded. */
    HlEntry aot_only[] = { { aot_key, unstamped, sizeof unstamped }, { 0, 0, 0 } };
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    hl_vfs_init(&vfs, aot_only, NULL);
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), HL_WASM_ERR_LOAD);
    hl_cap_wasm_destroy(&cache);

    /* From disk in development: the same. */
    char dir[512];
    ASSERT_EQ(mk_compute_app(dir, sizeof dir), 0);
    const char *archs[] = { "x86_64", "aarch64" };
    for (int k = 0; k < 2; k++) {
        char p[700];
        snprintf(p, sizeof p, "%s/compute/echo.aot.%s", dir, archs[k]);
        ASSERT_EQ(wr_file(p, unstamped, sizeof unstamped), 0);
    }
    static const HlEntry none[] = { { 0, 0, 0 } };
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    hl_vfs_init(&vfs, none, dir);
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, dir), 0);
    pthread_mutex_lock(&cache.pool_mutex);
    is_aot = cache.count == 1 ? cache.modules[0].is_aot : -1;
    pthread_mutex_unlock(&cache.pool_mutex);
    EXPECT_EQ(is_aot, 0);
    hl_cap_wasm_destroy(&cache);
    for (int k = 0; k < 2; k++) {
        char p[700];
        snprintf(p, sizeof p, "%s/compute/echo.aot.%s", dir, archs[k]);
        unlink(p);
    }
}

/* Counts the loader's "holds AOT" refusals (log.c has no callback removal,
 * so it is registered once and stays). */
static int g_aot_as_wasm_refusals;
static void count_aot_as_wasm(log_Event *ev)
{
    if (ev->fmt && strstr(ev->fmt, "holds AOT"))
        g_aot_as_wasm_refusals++;
}
static int count_aot_as_wasm_on(void)
{
    static int registered;
    if (registered) return 0;
    if (log_add_callback(count_aot_as_wasm, NULL, LOG_ERROR) != 0) return -1;
    registered = 1;
    return 0;
}

/* Round 7 (L2): WAMR loads AOT code from any file whose bytes start "\0aot",
 * whatever its name. AOT bytes saved as compute/<name>.wasm used to load
 * without the stamp check (is_aot = 0): the loader classifies by magic. */
UTEST(hl_cap_wasm, unstamped_aot_named_wasm_refused)
{
    ASSERT_EQ(count_aot_as_wasm_on(), 0);
    static uint8_t unstamped[128];
    mk_aot_head(unstamped, sizeof unstamped, 0, 0);

    /* Embedded. */
    HlEntry as_wasm[] = {
        { "compute/echo.wasm", unstamped, sizeof unstamped },
        { 0, 0, 0 }
    };
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, as_wasm, NULL);
    int before = g_aot_as_wasm_refusals;
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), HL_WASM_ERR_LOAD);
    EXPECT_EQ(g_aot_as_wasm_refusals, before + 1);
    hl_cap_wasm_destroy(&cache);

    /* From disk in development. */
    char dir[512];
    ASSERT_EQ(mk_compute_app(dir, sizeof dir), 0);
    char p[700];
    snprintf(p, sizeof p, "%s/compute/echo.wasm", dir);
    ASSERT_EQ(wr_file(p, unstamped, sizeof unstamped), 0);
    static const HlEntry none[] = { { 0, 0, 0 } };
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    hl_vfs_init(&vfs, none, dir);
    before = g_aot_as_wasm_refusals;
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, dir), HL_WASM_ERR_LOAD);
    EXPECT_EQ(g_aot_as_wasm_refusals, before + 1);
    hl_cap_wasm_destroy(&cache);

    /* Bytecode under the same name still loads, in the interpreter. */
    ASSERT_EQ(wr_file(p, echo_wasm, echo_wasm_len), 0);
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    hl_vfs_init(&vfs, none, dir);
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, dir), 0);
    hl_cap_wasm_destroy(&cache);
}

/* Audit 19 (H3): no AOT is loaded from disk, stamp or none. A STAMPED
 * compute/<name>.aot.<arch> in the app tree - native code from whoever wrote
 * the tree, a cloned repository - was run by `hull dev` / `hull test`; the
 * stamp only says the file claims to come from Hull's wamrc. Now the .wasm
 * beside it runs in the interpreter, and stamped AOT under the .wasm name is
 * refused. Embedded AOT (from `hull build`) still loads. */
UTEST(hl_cap_wasm, stamped_aot_never_loaded_from_disk)
{
    ASSERT_EQ(count_aot_as_wasm_on(), 0);
    /* A stamped head with no valid body: WAMR fails to load it, so before
     * the fix the load below returned HL_WASM_ERR_LOAD instead of falling
     * back to the .wasm. */
    static uint8_t stamped[128];
    mk_aot_head(stamped, sizeof stamped, HL_AOT_STAMP_LOW_OK, HL_AOT_STAMP_MAGIC);
    ASSERT_EQ(hl_aot_stamp_check(stamped, sizeof stamped), HL_AOT_STAMP_OK);

    char dir[512];
    ASSERT_EQ(mk_compute_app(dir, sizeof dir), 0);
    const char *archs[] = { "x86_64", "aarch64" };
    for (int k = 0; k < 2; k++) {
        char p[700];
        snprintf(p, sizeof p, "%s/compute/echo.aot.%s", dir, archs[k]);
        ASSERT_EQ(wr_file(p, stamped, sizeof stamped), 0);
    }
    static const HlEntry none[] = { { 0, 0, 0 } };
    HlWasmCache cache;
    HlVfs vfs;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    hl_vfs_init(&vfs, none, dir);
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, dir), 0);
    pthread_mutex_lock(&cache.pool_mutex);
    int is_aot = cache.count == 1 ? cache.modules[0].is_aot : -1;
    pthread_mutex_unlock(&cache.pool_mutex);
    EXPECT_EQ(is_aot, 0);
    void *out = NULL; size_t out_len = 0; const char *err = NULL;
    EXPECT_EQ(hl_cap_wasm_call(&cache, "echo", "hi", 2, &out, &out_len,
                               NULL, NULL, NULL, &vfs, dir, NULL, &err), 0);
    EXPECT_EQ(out_len, (size_t)2);
    free(out);
    hl_cap_wasm_destroy(&cache);
    for (int k = 0; k < 2; k++) {
        char p[700];
        snprintf(p, sizeof p, "%s/compute/echo.aot.%s", dir, archs[k]);
        unlink(p);
    }

    /* Stamped AOT saved as the .wasm: refused, not loaded. */
    char p[700];
    snprintf(p, sizeof p, "%s/compute/echo.wasm", dir);
    ASSERT_EQ(wr_file(p, stamped, sizeof stamped), 0);
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    hl_vfs_init(&vfs, none, dir);
    int before = g_aot_as_wasm_refusals;
    EXPECT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, dir), HL_WASM_ERR_LOAD);
    EXPECT_EQ(g_aot_as_wasm_refusals, before + 1);
    hl_cap_wasm_destroy(&cache);
    unlink(p);
}

/* Audit 20: an async binding refuses an input over the call's limit BEFORE
 * it copies it, by the limit the call paths apply (per-call, then the
 * instance's default, then the global default, clamped to the module's I/O
 * ceiling); and the copy holds its bytes on the VM's off-heap total, under
 * the heap limit, until the op gives them back. */
UTEST(hl_cap_wasm, async_input_limit_and_offheap_hold)
{
    HlWasmCallOpts opts = {0};
    HlWasmModule m32 = {0}, m64 = {0};
    m64.is_memory64 = 1;
    EXPECT_EQ(hl_cap_wasm_input_limit(NULL, NULL, NULL),
              (uint64_t)HL_WASM_DEFAULT_MAX_INPUT);
    opts.max_input = 4096;
    EXPECT_EQ(hl_cap_wasm_input_limit(&m32, NULL, &opts), (uint64_t)4096);
    HlWasmInstance pi = {0};
    pi.default_max_input = 777;
    EXPECT_EQ(hl_cap_wasm_input_limit(&m32, &pi, NULL), (uint64_t)777);
    EXPECT_EQ(hl_cap_wasm_input_limit(&m32, &pi, &opts), (uint64_t)4096);
    opts.max_input = HL_WASM64_MAX_IO_SIZE;
    EXPECT_EQ(hl_cap_wasm_input_limit(&m32, NULL, &opts),
              (uint64_t)HL_WASM_MAX_IO_SIZE);
    EXPECT_EQ(hl_cap_wasm_input_limit(&m64, NULL, &opts),
              (uint64_t)HL_WASM64_MAX_IO_SIZE);

    size_t total = 0;
    HlOffheapHold a = {0}, b = {0};
    EXPECT_EQ(hl_offheap_take(&a, &total, 100, 60), 0);
    EXPECT_EQ(total, (size_t)60);
    EXPECT_EQ(hl_offheap_take(&b, &total, 100, 41), -1);   /* 101 > 100 */
    EXPECT_EQ(total, (size_t)60);
    EXPECT_EQ(hl_offheap_take(&b, &total, 100, 40), 0);
    EXPECT_EQ(hl_offheap_take(&b, &total, 100, 1), -1);
    hl_offheap_give(&b, 15);
    EXPECT_EQ(total, (size_t)85);
    hl_offheap_drop(&a);
    hl_offheap_drop(&a);                                    /* twice: no-op */
    EXPECT_EQ(total, (size_t)25);
    hl_offheap_drop(&b);
    EXPECT_EQ(total, (size_t)0);
    EXPECT_EQ(hl_offheap_take(&a, &total, 0, (size_t)1 << 40), 0); /* no limit */
    hl_offheap_drop(&a);
    EXPECT_EQ(total, (size_t)0);
}

/* Audit 29: an async op's output is taken on its VM's in-flight result total
 * (opts.result_hold) before the host copies it; past the ceiling the call
 * fails "result too large" and nothing stays taken. Both call paths. */
UTEST(hl_cap_wasm, async_result_hold)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);
    const char *input = "0123456789abcdef";   /* 16 bytes, echoed */
    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        &cache, "echo", NULL, &vfs, NULL, NULL, &err);
    ASSERT_NE(pi, NULL);

    for (int path = 0; path < 2; path++) {
        size_t total = 0;
        HlOffheapShared hold;
        hl_offheap_shared_init(&hold, &total, 24);
        HlWasmCallOpts opts = {0};
        opts.result_hold = &hold;
        void *out = NULL;
        size_t out_len = 0;
        err = NULL;
        int rc = path == 0
            ? hl_cap_wasm_call(&cache, "echo", input, 16, &out, &out_len,
                               &opts, NULL, NULL, &vfs, NULL, NULL, &err)
            : hl_cap_wasm_instance_call(pi, input, 16, &out, &out_len,
                                        &opts, NULL, NULL, NULL, &err);
        ASSERT_EQ(rc, 0);
        EXPECT_EQ(out_len, (size_t)16);
        EXPECT_EQ(total, (size_t)16);
        EXPECT_EQ(hold.bytes, (size_t)16);
        free(out);
        out = NULL;

        /* 16 more would take the total to 32 > 24: refused. */
        HlOffheapShared hold2;
        hl_offheap_shared_init(&hold2, &total, 24);
        opts.result_hold = &hold2;
        err = NULL;
        rc = path == 0
            ? hl_cap_wasm_call(&cache, "echo", input, 16, &out, &out_len,
                               &opts, NULL, NULL, &vfs, NULL, NULL, &err)
            : hl_cap_wasm_instance_call(pi, input, 16, &out, &out_len,
                                        &opts, NULL, NULL, NULL, &err);
        EXPECT_NE(rc, 0);
        EXPECT_TRUE(err && strcmp(err, "result too large") == 0);
        EXPECT_EQ(out, NULL);
        EXPECT_EQ(total, (size_t)16);
        EXPECT_EQ(hold2.bytes, (size_t)0);

        hl_offheap_shared_drop(&hold);
        hl_offheap_shared_drop(&hold);   /* twice: no-op */
        EXPECT_EQ(total, (size_t)0);
    }

    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
}

/* ── Audit 41 ─────────────────────────────────────────────────────────
 *
 * M1: compute.stream charged each chunk 1 + input/64 only; the guest's
 * output (up to max_output a chunk, whatever the input) was copied and
 * written uncharged. The after_chunk hook sees each chunk's output length
 * BEFORE it is copied, written or handed on, and stops the stream. */
typedef struct { int calls; size_t bytes; size_t stop_after; } A41OutHook;

static int a41_after_chunk(size_t out_len, void *ud)
{
    A41OutHook *h = (A41OutHook *)ud;
    h->calls++;
    h->bytes += out_len;
    return h->bytes > h->stop_after;
}

static int a41_cb(const void *data, size_t len, uint32_t idx, int is_last,
                  void *ud)
{
    (void)data; (void)len; (void)idx; (void)is_last;
    (*(int *)ud)++;
    return 0;
}

UTEST(hl_cap_wasm, a41_stream_after_chunk_sees_output_before_it_is_kept)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    static char input[4096];
    memset(input, 'x', sizeof input);
    HlStreamInput in = {0};
    in.kind = HL_STREAM_IN_BUFFER;
    in.buffer.data = input;
    in.buffer.len = sizeof input;   /* 16 chunks of 256 */

    /* Every chunk's output is reported with its length. */
    void *out_data = NULL;
    size_t out_len = 0;
    HlStreamOutput out = {0};
    out.kind = HL_STREAM_OUT_BUFFER;
    out.buffer.data = &out_data;
    out.buffer.len = &out_len;
    HlStreamOpts opts = {0};
    opts.chunk_size = 256;
    opts.call_opts.max_output = 1024;
    A41OutHook h = { 0, 0, (size_t)-1 };
    opts.after_chunk = a41_after_chunk;
    opts.after_chunk_ud = &h;
    HlStreamResult res = {0};
    const char *err = NULL;
    ASSERT_EQ(hl_cap_wasm_stream(&cache, "big_out", &in, &out, &opts,
                                 NULL, &vfs, NULL, NULL, &res, &err), 0);
    EXPECT_EQ(h.calls, 16);
    EXPECT_EQ(h.bytes, (size_t)16 * 1024);
    EXPECT_EQ(out_len, (size_t)16 * 1024);
    free(out_data);
    out_data = NULL;
    out_len = 0;

    /* A stop keeps nothing of the chunk that tripped it. */
    h = (A41OutHook){ 0, 0, 2 * 1024 };
    err = NULL;
    memset(&res, 0, sizeof res);
    EXPECT_NE(hl_cap_wasm_stream(&cache, "big_out", &in, &out, &opts,
                                 NULL, &vfs, NULL, NULL, &res, &err), 0);
    EXPECT_TRUE(err && strcmp(err, "stopped") == 0);
    EXPECT_EQ(h.calls, 3);
    EXPECT_EQ(res.total_output, (size_t)2 * 1024);
    EXPECT_EQ(out_data, NULL);

    /* A callback output is not called for the chunk that tripped. */
    h = (A41OutHook){ 0, 0, 2 * 1024 };
    int cb_calls = 0;
    HlStreamOutput cbo = {0};
    cbo.kind = HL_STREAM_OUT_CALLBACK;
    cbo.callback.fn = a41_cb;
    cbo.callback.user_data = &cb_calls;
    err = NULL;
    EXPECT_NE(hl_cap_wasm_stream(&cache, "big_out", &in, &cbo, &opts,
                                 NULL, &vfs, NULL, NULL, &res, &err), 0);
    EXPECT_EQ(cb_calls, 2);

    hl_cap_wasm_destroy(&cache);
}

/* M2: a guest's LOG lines are capped per guest run - 64 lines / 64 KiB -
 * and the rest dropped with one "suppressed N log lines" record at the end
 * of the run. One compute.call wrote 512,801 lines before its timeout. */
static atomic_int a41_log_on;
static atomic_int a41_log_lines;
static atomic_int a41_log_suppressed;
static void a41_log_cb(log_Event *ev)
{
    if (!atomic_load(&a41_log_on) || !ev->fmt) return;
    if (ev->level == LOG_INFO && strncmp(ev->fmt, "[wasm] %s", 9) == 0)
        atomic_fetch_add(&a41_log_lines, 1);
    else if (ev->level == LOG_WARN && strstr(ev->fmt, "suppressed"))
        atomic_fetch_add(&a41_log_suppressed, 1);
}

UTEST(hl_cap_wasm, a41_guest_log_lines_are_capped_per_call)
{
    static int registered;
    if (!registered) {
        ASSERT_EQ(log_add_callback(a41_log_cb, NULL, LOG_TRACE), 0);
        registered = 1;
    }
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);
    log_set_quiet(true);

    /* Two one-shot calls: each gets its own quota. */
    for (int i = 0; i < 2; i++) {
        atomic_store(&a41_log_lines, 0);
        atomic_store(&a41_log_suppressed, 0);
        atomic_store(&a41_log_on, 1);
        void *out = NULL;
        size_t out_len = 0;
        const char *err = NULL;
        int rc = hl_cap_wasm_call(&cache, "logspam", "x", 1, &out, &out_len,
                                  NULL, NULL, NULL, &vfs, NULL, NULL, &err);
        atomic_store(&a41_log_on, 0);
        EXPECT_EQ(rc, 0);
        free(out);
        EXPECT_EQ(atomic_load(&a41_log_lines), HL_WASM_LOG_MAX_LINES);
        EXPECT_EQ(atomic_load(&a41_log_suppressed), 1);
    }

    /* A persistent instance's call too. */
    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        &cache, "logspam", NULL, &vfs, NULL, NULL, &err);
    ASSERT_NE(pi, NULL);
    atomic_store(&a41_log_lines, 0);
    atomic_store(&a41_log_suppressed, 0);
    atomic_store(&a41_log_on, 1);
    void *out = NULL;
    size_t out_len = 0;
    EXPECT_EQ(hl_cap_wasm_instance_call(pi, "x", 1, &out, &out_len,
                                        NULL, NULL, NULL, NULL, &err), 0);
    atomic_store(&a41_log_on, 0);
    free(out);
    EXPECT_EQ(atomic_load(&a41_log_lines), HL_WASM_LOG_MAX_LINES);
    EXPECT_EQ(atomic_load(&a41_log_suppressed), 1);
    hl_cap_wasm_instance_destroy(pi);

    log_set_quiet(false);
    hl_cap_wasm_destroy(&cache);
}

int main(int argc, const char *const argv[])
{
    hl_log_make_threadsafe();
    return utest_main(argc, argv);
}
