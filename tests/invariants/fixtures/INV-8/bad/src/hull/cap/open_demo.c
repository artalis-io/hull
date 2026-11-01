/* INV-8 bad: PRAGMA journal_mode=WAL (in hl_cap_db_init) maps the -shm
 * before the file is registered. */
int demo_open(const char *path, sqlite3 **db)
{
    if (sqlite3_open_v2(path, db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK)
        return -1;
    if (hl_cap_db_init(*db) != 0) return -1;
    return hl_db_live_file_add(path);
}
