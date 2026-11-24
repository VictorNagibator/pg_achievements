-- Counters of utility commands (ProcessUtility_hook).
SELECT pga_test_role('t_ddl', true);
SET ROLE t_ddl;
\set QUIET on

EXPLAIN (COSTS OFF) SELECT 1;
-- the plan output differs between versions, only the notice matters
\o /dev/null
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT 1;
\o

CREATE TABLE d1 (id int, v int);
CREATE TABLE IF NOT EXISTS d1 (id int);   -- exists: not counted
CREATE TABLE d2 AS SELECT 1 AS id;
CREATE TABLE IF NOT EXISTS d2 AS SELECT 1 AS id;   -- exists: not counted
CREATE INDEX d1_v ON d1 (v);
CREATE INDEX IF NOT EXISTS d1_v ON d1 (v);         -- exists: not counted
CREATE INDEX CONCURRENTLY d1_id ON d1 (id);
CREATE FUNCTION d_trg() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$;
CREATE PROCEDURE d_proc() LANGUAGE sql AS $$ SELECT 1 $$;
CREATE TRIGGER d1_trg BEFORE INSERT ON d1 FOR EACH ROW EXECUTE FUNCTION d_trg();
CREATE TABLE dp (id int) PARTITION BY RANGE (id);
CREATE TABLE dp_1 PARTITION OF dp FOR VALUES FROM (0) TO (100);
TRUNCATE d1;
VACUUM d1;              -- plain VACUUM: not counted
VACUUM (FULL) d1;
CREATE EXTENSION IF NOT EXISTS plpgsql;    -- exists: not counted

COMMENT ON TABLE d1 IS 'commented';
CREATE POLICY d1_pol ON d1 USING (true);
CREATE MATERIALIZED VIEW d_mv AS SELECT 1 AS id;
CREATE UNIQUE INDEX d_mv_id ON d_mv (id);
REFRESH MATERIALIZED VIEW d_mv;               -- not concurrently: not counted
REFRESH MATERIALIZED VIEW CONCURRENTLY d_mv;
LISTEN d_chan;
UNLISTEN d_chan;                              -- not counted
NOTIFY d_other_chan;   -- nobody listens: no message with a PID in the output

-- lock_exclusive: LOCK TABLE takes ACCESS EXCLUSIVE unless told otherwise
BEGIN;
LOCK TABLE d1 IN SHARE MODE;
LOCK TABLE d1;
COMMIT;

-- rollback and rollback_to_savepoint
ROLLBACK;               -- no transaction: just a warning, not counted
BEGIN;
SAVEPOINT s;
ROLLBACK TO SAVEPOINT s;
ROLLBACK;

-- emoji_name: an emoji, quoted or not; other non-ASCII letters do not count
CREATE TABLE "elephant 🐘" (id int);
CREATE TABLE d_🐘 AS SELECT 1 AS id;
CREATE TABLE "naïve" (id int);

-- long_name: the name is truncated to 63 bytes; exactly 63 is fine
CREATE TABLE public.d_this_table_name_is_far_too_long_to_fit_into_sixty_three_bytes_ (id int);
CREATE TABLE d_this_table_name_is_exactly_sixty_three_bytes_long_believe_me_ (id int);

-- disable_attempt: only switching pg_achievements.enabled off
ALTER SYSTEM SET pg_achievements.enabled = on;
ALTER SYSTEM SET pg_achievements.enabled = off;
ALTER SYSTEM RESET pg_achievements.enabled;

-- DROP TABLE counts every dropped table
DROP TABLE d1, d2, "elephant 🐘", d_🐘, "naïve",
    d_this_table_name_is_far_too_long_to_fit_into_sixty_three_bytes,
    d_this_table_name_is_exactly_sixty_three_bytes_long_believe_me_;
DROP MATERIALIZED VIEW d_mv;
DROP TABLE IF EXISTS d_missing;            -- nothing dropped
DROP TABLE dp;

CREATE DATABASE pga_drop_me;
DROP DATABASE pga_drop_me;
DROP DATABASE IF EXISTS pga_drop_me;       -- nothing dropped

-- DDL on Friday evening; Friday morning and Saturday do not count
SET pg_achievements.test_now = '2026-11-20 09:00:00-08';
CREATE TABLE fri_morning (id int);
SET pg_achievements.test_now = '2026-11-21 19:00:00-08';
CREATE TABLE saturday (id int);
SET pg_achievements.test_now = '2026-11-20 18:30:00-08';
CREATE TABLE fri_evening (id int);
SELECT 1 AS not_ddl;
RESET pg_achievements.test_now;
DROP TABLE fri_morning, saturday, fri_evening;

-- mistakes of the user fail the command, but must not look like internal
-- errors of the extension: no WARNING, and the session is still tracked
\set VERBOSITY terse
EXPLAIN (ANALYZE maybe) SELECT 1;
VACUUM (FULL maybe) d1;
DROP TABLE IF EXISTS a.b.c.d;
DROP TABLE IF EXISTS pga_no_such_db.public.t;
CREATE TABLE IF NOT EXISTS pga_no_such_db.public.t (id int);
\set VERBOSITY default
EXPLAIN (ANALYZE off, COSTS OFF) SELECT 1;   -- counts explain_any only

\set QUIET off
SELECT * FROM pga_counters();
RESET ROLE;
