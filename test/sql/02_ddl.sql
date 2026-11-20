-- Counters of utility commands (ProcessUtility_hook).
SELECT pga_test_role('t_ddl', true);
SET ROLE t_ddl;
\set QUIET on

EXPLAIN (COSTS OFF) SELECT 1;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT 1;

CREATE TABLE d1 (id int, v int);
CREATE TABLE IF NOT EXISTS d1 (id int);   -- exists: not counted
CREATE TABLE d2 AS SELECT 1 AS id;
CREATE INDEX d1_v ON d1 (v);
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

-- DROP TABLE counts every dropped table
DROP TABLE d1, d2;
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

\set QUIET off
SELECT * FROM pga_counters();
RESET ROLE;
