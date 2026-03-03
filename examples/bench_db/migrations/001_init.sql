CREATE TABLE events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL,
    payload TEXT,
    ts INTEGER NOT NULL
);

CREATE INDEX idx_events_ts ON events (ts DESC);
