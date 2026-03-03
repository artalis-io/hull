CREATE TABLE webhooks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    url TEXT NOT NULL,
    events TEXT NOT NULL,
    active INTEGER DEFAULT 1,
    created_at INTEGER
);

CREATE TABLE event_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_type TEXT NOT NULL,
    payload TEXT NOT NULL,
    created_at INTEGER
);

CREATE TABLE deliveries (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    webhook_id INTEGER NOT NULL,
    event_id INTEGER NOT NULL,
    status INTEGER,
    response_body TEXT,
    created_at INTEGER,
    FOREIGN KEY (webhook_id) REFERENCES webhooks(id)
);
