/*
 * fuzz_migrate_name_stubs.c: the Hull symbols migrate.c references besides
 * hl_migrate_embedded_name, for fuzz_migrate_name. The fuzzed predicate never
 * calls them; the real ones pull in the DB layer, crypto and the VFS. No Hull
 * header is included, so the stub signatures cannot conflict. Each aborts if
 * reached.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdlib.h>

void hl_cap_crypto_sha256(void);
void hl_cap_crypto_sha256(void) { abort(); }
void hl_cap_db_check_namespace(void);
void hl_cap_db_check_namespace(void) { abort(); }
void hl_db_budget_restore(void);
void hl_db_budget_restore(void) { abort(); }
void hl_db_budget_swap(void);
void hl_db_budget_swap(void) { abort(); }
void hl_hex_encode(void);
void hl_hex_encode(void) { abort(); }
void hl_vfs_disk_gate_check(void);
void hl_vfs_disk_gate_check(void) { abort(); }
void hl_vfs_prefix(void);
void hl_vfs_prefix(void) { abort(); }
void hl_db_txn_note(void);
void hl_db_txn_note(void) { abort(); }
