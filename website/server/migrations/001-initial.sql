CREATE TABLE games (
  sgdb_id bigint PRIMARY KEY,
  slug text NOT NULL UNIQUE,
  title text NOT NULL,
  catalog jsonb NOT NULL
);
CREATE TABLE reports (
  id uuid PRIMARY KEY,
  game_id bigint NOT NULL REFERENCES games(sgdb_id),
  fields jsonb NOT NULL,
  created_at timestamptz NOT NULL DEFAULT now(),
  triage text NOT NULL DEFAULT 'new' CHECK (triage IN ('new','reviewed','needs-info','hidden')),
  internal_note text NOT NULL DEFAULT ''
);
CREATE INDEX reports_game_created ON reports(game_id, created_at DESC);
CREATE INDEX reports_created ON reports(created_at DESC);
CREATE TABLE private_logs (
  report_id uuid PRIMARY KEY REFERENCES reports(id) ON DELETE CASCADE,
  content bytea NOT NULL CHECK (octet_length(content) <= 524288),
  expires_at timestamptz NOT NULL DEFAULT now() + interval '90 days'
);
CREATE INDEX private_logs_expiry ON private_logs(expires_at);
CREATE TABLE sessions (
  token_hash text PRIMARY KEY,
  github_id text NOT NULL,
  github_login text NOT NULL,
  expires_at timestamptz NOT NULL
);
CREATE TABLE oauth_attempts (
  state_hash text PRIMARY KEY,
  expires_at timestamptz NOT NULL
);
CREATE TABLE rate_limits (
  key text PRIMARY KEY,
  count integer NOT NULL,
  expires_at timestamptz NOT NULL
);
CREATE TABLE audit_events (
  id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  actor text NOT NULL,
  action text NOT NULL,
  report_id uuid,
  created_at timestamptz NOT NULL DEFAULT now()
);
