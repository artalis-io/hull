-- Users table for the auth-flows example app. Schema kept minimal
-- and convention-driven so the auth-flows callbacks line up 1:1
-- with column names:
--
--   user.id              -> primary key passed back to callbacks
--   user.email           -> looked up by user_find_by_email
--   user.password_hash   -> nullable so magic-link-only accounts work
--   user.email_verified  -> flipped to 1 by user_set_email_verified
--
-- Real apps almost always add more columns (display name, role,
-- locale, created_by, etc.) - auth-flows doesn't care, just keep
-- these four.
CREATE TABLE IF NOT EXISTS users (
    id              TEXT PRIMARY KEY,
    email           TEXT NOT NULL UNIQUE,
    password_hash   TEXT,
    email_verified  INTEGER NOT NULL DEFAULT 0,
    created_at      INTEGER NOT NULL,
    updated_at      INTEGER NOT NULL
);
