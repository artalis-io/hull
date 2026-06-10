-- Migration: 002_attachments
-- Per-entry photo attachments. attachment_id references the opaque id
-- returned by hull/attachment@1's store(); the attachment module
-- handles refcount + content-addressed blob storage underneath, so
-- this join table just owns the "which entry holds which attachment"
-- relationship.

CREATE TABLE IF NOT EXISTS entry_attachments (
    entry_id       INTEGER NOT NULL,
    attachment_id TEXT NOT NULL,
    created_at    INTEGER NOT NULL,
    PRIMARY KEY (entry_id, attachment_id)
);

CREATE INDEX IF NOT EXISTS idx_entry_attachments_entry_id
    ON entry_attachments(entry_id);
