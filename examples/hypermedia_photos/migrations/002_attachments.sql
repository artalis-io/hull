-- Migration: 002_attachments
-- Per-entry photo attachments. attachment_id references the opaque id
-- returned by hull/attachment@1's store(); the attachment module
-- handles refcount + content-addressed blob storage underneath, so
-- this join table just owns the "which entry holds which attachment"
-- relationship.
--
-- ON DELETE CASCADE is a defensive backstop: when an entry is
-- deleted, SQLite auto-removes the join rows. The app handler still
-- enumerates the attachments BEFORE the parent delete (so it can
-- call attachment.delete(id) to decrement each refcount). The
-- cascade only fires if the app forgets that step or a future caller
-- deletes the parent directly.
--
-- NOTE: SQLite ignores FK declarations unless `PRAGMA foreign_keys =
-- ON` is set per connection. The declaration is parsed-but-inert
-- otherwise and acts as schema documentation; Hull's per-connection
-- pragma defaults govern actual enforcement.

CREATE TABLE IF NOT EXISTS entry_attachments (
    entry_id      INTEGER NOT NULL,
    attachment_id TEXT NOT NULL,
    created_at    INTEGER NOT NULL,
    PRIMARY KEY (entry_id, attachment_id),
    FOREIGN KEY (entry_id) REFERENCES entries(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_entry_attachments_entry_id
    ON entry_attachments(entry_id);
