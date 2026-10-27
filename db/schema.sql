-- ResQ Dispatch - SQLite schema. Safe to run repeatedly (IF NOT EXISTS).
-- Timestamps are stored as UTC ISO-8601 text, e.g. 2026-10-26T09:15:00Z.

CREATE TABLE IF NOT EXISTS meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS users (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  name          TEXT NOT NULL,
  username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
  email         TEXT NOT NULL DEFAULT '',
  password_hash TEXT NOT NULL,          -- PBKDF2-HMAC-SHA256, hex
  salt          TEXT NOT NULL,          -- 16 random bytes, hex
  iterations    INTEGER NOT NULL,
  role          TEXT NOT NULL CHECK (role IN ('admin','dispatcher')),
  created_at    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now'))
);
CREATE UNIQUE INDEX IF NOT EXISTS ux_users_email
  ON users(email COLLATE NOCASE) WHERE email <> '';

CREATE TABLE IF NOT EXISTS sessions (
  token_hash TEXT PRIMARY KEY,          -- SHA-256 of the bearer token (token itself is never stored)
  user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),
  expires_at TEXT NOT NULL
);

-- Road graph used by Dijkstra.
CREATE TABLE IF NOT EXISTS nodes (
  id      INTEGER PRIMARY KEY,
  name    TEXT NOT NULL UNIQUE,
  aliases TEXT NOT NULL DEFAULT ''      -- comma separated alternative spellings
);
CREATE TABLE IF NOT EXISTS edges (
  a  INTEGER NOT NULL REFERENCES nodes(id),
  b  INTEGER NOT NULL REFERENCES nodes(id),
  km REAL NOT NULL CHECK (km > 0),
  PRIMARY KEY (a, b)
);

CREATE TABLE IF NOT EXISTS vehicles (
  id       TEXT PRIMARY KEY,
  type     TEXT NOT NULL CHECK (type IN ('ALS','BLS')),
  status   TEXT NOT NULL CHECK (status IN ('Available','On route','On Duty','Maintenance')),
  location TEXT NOT NULL,
  node_id  INTEGER REFERENCES nodes(id)
);

CREATE TABLE IF NOT EXISTS emergencies (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  type       TEXT NOT NULL,
  priority   TEXT NOT NULL CHECK (priority IN ('High','Medium','Low')),
  location   TEXT NOT NULL,
  node_id    INTEGER REFERENCES nodes(id),
  patient    TEXT NOT NULL DEFAULT '',
  details    TEXT NOT NULL DEFAULT '',
  status     TEXT NOT NULL CHECK (status IN ('Waiting','Dispatched','Completed')),
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),
  created_by INTEGER REFERENCES users(id) ON DELETE SET NULL
);
CREATE INDEX IF NOT EXISTS ix_emergencies_status ON emergencies(status);

CREATE TABLE IF NOT EXISTS dispatches (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  emergency_id  INTEGER NOT NULL REFERENCES emergencies(id),
  vehicle_id    TEXT NOT NULL REFERENCES vehicles(id),
  status        TEXT NOT NULL CHECK (status IN ('In Progress','Completed')),
  distance_km   REAL,                   -- NULL when no road route could be computed
  route         TEXT NOT NULL DEFAULT '',
  method        TEXT NOT NULL DEFAULT 'manual',   -- dijkstra | manual | fallback
  dispatched_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),
  completed_at  TEXT,
  dispatched_by INTEGER REFERENCES users(id) ON DELETE SET NULL
);
CREATE INDEX IF NOT EXISTS ix_dispatches_status ON dispatches(status);

CREATE TABLE IF NOT EXISTS notifications (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  title      TEXT NOT NULL,
  message    TEXT NOT NULL,
  icon       TEXT NOT NULL DEFAULT '',
  is_read    INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now'))
);
CREATE INDEX IF NOT EXISTS ix_notifications_user ON notifications(user_id, id);
