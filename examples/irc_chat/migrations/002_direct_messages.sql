CREATE TABLE IF NOT EXISTS direct_messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sender_id INTEGER NOT NULL,
    recipient_id INTEGER NOT NULL,
    encrypted_for_recipient TEXT NOT NULL,
    encrypted_for_sender TEXT NOT NULL,
    nonce TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    FOREIGN KEY (sender_id) REFERENCES users(id),
    FOREIGN KEY (recipient_id) REFERENCES users(id)
);

CREATE INDEX IF NOT EXISTS idx_dm_sender ON direct_messages(sender_id, recipient_id, created_at);
CREATE INDEX IF NOT EXISTS idx_dm_recipient ON direct_messages(recipient_id, sender_id, created_at);
