-- Migration: 002_attachments
-- Per-todo photo attachments. attachment_id references the opaque id
-- returned by hull/attachment@1's store(); the attachment module
-- handles refcount + content-addressed blob storage underneath, so
-- this join table just owns the "which todo holds which attachment"
-- relationship.

CREATE TABLE IF NOT EXISTS todo_attachments (
    todo_id       INTEGER NOT NULL,
    attachment_id TEXT NOT NULL,
    created_at    INTEGER NOT NULL,
    PRIMARY KEY (todo_id, attachment_id)
);

CREATE INDEX IF NOT EXISTS idx_todo_attachments_todo_id
    ON todo_attachments(todo_id);
