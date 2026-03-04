CREATE TABLE IF NOT EXISTS email_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    recipient TEXT NOT NULL,
    subject TEXT NOT NULL,
    body TEXT NOT NULL,
    content_type TEXT DEFAULT 'text/plain',
    cc TEXT,
    reply_to TEXT,
    status TEXT NOT NULL,
    error TEXT,
    created_at INTEGER
);
