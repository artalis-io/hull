/* INV-8 good: registered before anything reads the database; an in-memory
 * database has no file to register. */
int demo_open(const char *path, sqlite3 **db)
{
    if (sqlite3_open(path, db) != SQLITE_OK) return -1;
    if (hl_db_live_file_add(path) != 0) return -1;
    return hl_cap_db_init(*db);
}

int demo_open_mem(sqlite3 **db)
{
    if (sqlite3_open(":memory:", db) != SQLITE_OK) return -1;
    return hl_cap_db_init(*db);
}
