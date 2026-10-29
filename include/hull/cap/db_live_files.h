/*
 * hull/cap/db_live_files.h - the database files SQLite holds open
 *
 * SQLite maps a WAL database's index (`<db>-shm`) MAP_SHARED in every
 * connection, and that mapping is not one of Hull's (the fs.mmap SIGBUS guard
 * does not cover it): truncating the file in place made SQLite's next read
 * of the index fault and killed the process - `compute.stream(..., {file =
 * "data/app.db-shm"})` and an `fs.write` through an in-root symlink both did
 * (audit 33). Replacing or removing a live `-wal` / `-shm` / `-journal`, or the
 * database itself, is no better: a connection that opens later builds a WAL
 * index of its own beside the one the others still use, and a removed `-wal`
 * takes committed transactions with it.
 *
 * Every connection the SQLite backend opens registers its database file here
 * (refcounted by path, process-wide, any thread) right after sqlite3_open,
 * before anything maps the -shm (audit 34), and the fs capability
 * refuses to rewrite, replace or delete a file whose device and inode are the
 * database's or one of its sidecars'.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_LIVE_FILES_H
#define HL_CAP_DB_LIVE_FILES_H

#include <sys/stat.h>

/* Register / unregister the database file at @p path (an absolute path, as
 * sqlite3_db_filename gives it). NULL / "" (an in-memory database) is
 * ignored. Each add is paired with one remove. Returns 0, -1 when it could
 * not be recorded (out of memory). */
int  hl_db_live_file_add(const char *path);
void hl_db_live_file_remove(const char *path);

/* 1 when @p st (a regular file's stat) is a registered database file or one
 * of its -wal / -shm / -journal sidecars, as they are on disk now. */
int  hl_db_live_file_match(const struct stat *st);

/* The registry's lock, for a check that must be atomic with what it guards
 * (audit 34): the fs capability holds it from the stat it matches through
 * the one destructive syscall (ftruncate / renameat / unlinkat), so a
 * connection registering meanwhile (hl_db_live_file_add takes the same lock,
 * before SQLite maps anything) either sees the file already changed or makes
 * the check refuse. Held only around single syscalls; never across a SQLite
 * call. hl_db_live_file_match_locked is hl_db_live_file_match with the lock
 * already held. */
void hl_db_live_files_lock(void);
void hl_db_live_files_unlock(void);
int  hl_db_live_file_match_locked(const struct stat *st);

/* 1 when @p path's last component ends in "-wal", "-shm" or "-journal":
 * SQLite's sidecar names, refused for an in-place write whatever they are. */
int  hl_db_sidecar_name(const char *path);

/* The error string the fs capability reports for either refusal. */
#define HL_DB_LIVE_FILE_ERR "database_file_in_use"

#endif /* HL_CAP_DB_LIVE_FILES_H */
