CREATE TABLE IF NOT EXISTS files (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    uploader_id INTEGER NOT NULL,
    filename TEXT NOT NULL,
    size INTEGER NOT NULL,
    content TEXT NOT NULL,
    channel_id INTEGER,
    recipient_id INTEGER,
    nonce TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    FOREIGN KEY (uploader_id) REFERENCES users(id),
    FOREIGN KEY (channel_id) REFERENCES channels(id),
    FOREIGN KEY (recipient_id) REFERENCES users(id)
);

CREATE INDEX IF NOT EXISTS idx_files_channel ON files(channel_id, created_at);
CREATE INDEX IF NOT EXISTS idx_files_dm ON files(uploader_id, recipient_id, created_at);
CREATE INDEX IF NOT EXISTS idx_files_recipient ON files(recipient_id, uploader_id, created_at);
