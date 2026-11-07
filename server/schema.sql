-- Replayr accounts (Cloudflare D1 / SQLite). Apply with:
--   npx wrangler d1 execute replayr --remote --file=schema.sql
CREATE TABLE IF NOT EXISTS users (
  id            TEXT PRIMARY KEY,           -- random id
  provider      TEXT NOT NULL,              -- 'discord' | 'steam'
  provider_id   TEXT NOT NULL,              -- Discord user id / SteamID64
  display_name  TEXT NOT NULL,
  avatar_url    TEXT,
  quota_bytes   INTEGER NOT NULL DEFAULT 524288000,   -- 500 MB free per user
  used_bytes    INTEGER NOT NULL DEFAULT 0,
  created_at    INTEGER NOT NULL,
  banned        INTEGER NOT NULL DEFAULT 0,
  UNIQUE (provider, provider_id)
);

-- One-time codes handed to the app's loopback listener after the provider sign-in (PKCE-bound).
CREATE TABLE IF NOT EXISTS login_codes (
  code_hash     TEXT PRIMARY KEY,
  user_id       TEXT NOT NULL,
  challenge     TEXT NOT NULL,              -- base64url(SHA-256(verifier)) from the app
  expires_at    INTEGER NOT NULL
);

-- Pending sign-ins: state -> where to send the browser back to.
CREATE TABLE IF NOT EXISTS pending (
  state         TEXT PRIMARY KEY,
  port          INTEGER NOT NULL,
  app_state     TEXT NOT NULL,
  challenge     TEXT NOT NULL,
  expires_at    INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS sessions (
  refresh_hash  TEXT PRIMARY KEY,           -- SHA-256 of the refresh token; the token itself is never stored
  user_id       TEXT NOT NULL,
  device        TEXT,
  created_at    INTEGER NOT NULL,
  last_used_at  INTEGER NOT NULL,
  expires_at    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS sessions_user ON sessions (user_id);

CREATE TABLE IF NOT EXISTS clips (
  id            TEXT PRIMARY KEY,           -- short id used in share links
  user_id       TEXT NOT NULL,
  title         TEXT NOT NULL,
  privacy       TEXT NOT NULL DEFAULT 'unlisted',   -- 'public' | 'unlisted' | 'private'
  size_bytes    INTEGER NOT NULL,
  duration      REAL NOT NULL DEFAULT 0,
  status        TEXT NOT NULL DEFAULT 'uploading',  -- 'uploading' | 'ready' | 'removed'
  views         INTEGER NOT NULL DEFAULT 0,
  created_at    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS clips_user ON clips (user_id, created_at);

CREATE TABLE IF NOT EXISTS reports (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  clip_id       TEXT NOT NULL,
  reason        TEXT NOT NULL,
  created_at    INTEGER NOT NULL,
  ip_hash       TEXT
);
