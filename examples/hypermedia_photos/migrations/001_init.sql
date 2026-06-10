-- Migration: 001_init
-- Schema for the htmx entries demo.

CREATE TABLE IF NOT EXISTS entries (
    id    INTEGER PRIMARY KEY AUTOINCREMENT,
    title TEXT NOT NULL,
    done  INTEGER NOT NULL DEFAULT 0
);
