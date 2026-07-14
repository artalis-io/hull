// models/user.js — Persistence layer for the User resource.

import { crypto } from "hull:crypto";
import { db as dbModule } from "hull:db";
const db = dbModule.default();
import { time }   from "hull:time";

export function create(input) {
    // Random bytes → SHA-256 hex (64 chars). crypto.random returns
    // raw bytes; sha256 returns a hex string. Combined this gives a
    // cryptographically-strong opaque id with no separate hex-encoder.
    const id  = crypto.sha256(crypto.random(32));
    const now = time.now();
    db.exec(
        "INSERT INTO users (id, email, name, created_at) VALUES (?, ?, ?, ?)",
        [id, input.email, input.name, now]
    );
    return [findById(id), null];
}

export function findById(id) {
    const rows = db.query(
        "SELECT id, email, name, created_at FROM users WHERE id = ?",
        [id]
    );
    return rows[0] ?? null;
}

export function list(opts) {
    const limit = (opts?.limit) || 50;
    return db.query(
        "SELECT id, email, name, created_at FROM users " +
        "ORDER BY created_at DESC LIMIT ?",
        [limit]
    );
}

export function deleteById(id) {
    const rows = db.query("SELECT id FROM users WHERE id = ?", [id]);
    if (rows.length === 0) return false;
    db.exec("DELETE FROM users WHERE id = ?", [id]);
    return true;
}
