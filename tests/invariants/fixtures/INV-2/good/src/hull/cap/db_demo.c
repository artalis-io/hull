/* INV-2 good: the cap layer may call the SQLite engine. */
int hl_cap_db_demo(sqlite3 *db)
{
    return sqlite3_exec(db, "SELECT 1", NULL, NULL, NULL);
}
