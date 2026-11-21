-- Earn every builtin achievement with a single role, including the meta
-- achievements.  Repetitive events are generated with \gexec.
CREATE ROLE t_all SUPERUSER LOGIN;
\c - t_all
\set QUIET on
\o /dev/null

-- bronze
SELECT 1;
CREATE TABLE a_t (id int, j jsonb);
CREATE INDEX ON a_t (id);
CREATE FUNCTION a_f() RETURNS int LANGUAGE sql AS 'SELECT 1';
WITH x AS (SELECT 1) SELECT * FROM x;
SELECT 1 FROM a_t JOIN a_t b USING (id);
EXPLAIN SELECT 1;
SET pg_achievements.notify = off;   -- see 04_errors
SELECT 'SELEC ' || g FROM generate_series(1, 10) g \gexec
RESET pg_achievements.notify;
SELECT 'SELECT * FROM a_t' FROM generate_series(1, 1000) \gexec
SET pg_achievements.test_now = '2026-11-18 04:00:00-08';
SELECT 'SELECT 1' FROM generate_series(1, 20) \gexec
RESET pg_achievements.test_now;
SELECT 1 FROM a_t WHERE id = NULL;

-- silver
EXPLAIN ANALYZE SELECT 1;
WITH RECURSIVE r AS (SELECT 1 UNION SELECT 1 FROM r) SELECT * FROM r;
SELECT row_number() OVER () FROM a_t;
SELECT * FROM a_t, LATERAL (SELECT a_t.id) s;
SELECT j -> 'a', j -> 'b', j ->> 'c', j ->> 'd', j @> '{}', j ? 'e',
       j -> 'f', j -> 'g', j -> 'h', j -> 'i' FROM a_t;
SET pg_achievements.test_time_shift = 3601;
SELECT 1;
BEGIN;
ROLLBACK;
RESET pg_achievements.test_time_shift;
CREATE INDEX CONCURRENTLY ON a_t (j);
CREATE FUNCTION a_trg() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END';
CREATE TRIGGER a_trg BEFORE INSERT ON a_t FOR EACH ROW EXECUTE FUNCTION a_trg();
CREATE TABLE a_p (id int) PARTITION BY LIST (id);
CREATE EXTENSION pg_stat_statements;
DROP EXTENSION pg_stat_statements;
CREATE EXTENSION pg_stat_statements;
DROP EXTENSION pg_stat_statements;
CREATE EXTENSION pg_stat_statements;
DROP EXTENSION pg_stat_statements;

-- gold
UPDATE a_t SET id = id;
DELETE FROM a_t;
SET pg_achievements.notify = off;
SELECT 'DO $$ BEGIN RAISE EXCEPTION ''x'' USING ERRCODE = ''40P01''; END $$' FROM generate_series(1, 5) \gexec
DO $$ BEGIN RAISE EXCEPTION 'x' USING ERRCODE = '40001'; END $$;
RESET pg_achievements.notify;
SET pg_achievements.test_now = '2026-11-20 20:00:00-08';
CREATE TABLE a_friday (id int);
RESET pg_achievements.test_now;
CREATE TABLE a_big AS SELECT generate_series(1, 2000) AS id;
ANALYZE a_big;
SELECT count(*) FROM a_big;
SELECT pg_terminate_backend(-1), pg_terminate_backend(-1), pg_terminate_backend(-1),
       pg_terminate_backend(-1), pg_terminate_backend(-1), pg_terminate_backend(-1),
       pg_terminate_backend(-1), pg_terminate_backend(-1), pg_terminate_backend(-1),
       pg_terminate_backend(-1)
FROM generate_series(1, 0);
TRUNCATE a_t;
VACUUM FULL a_t;
SELECT 1 FROM a_t t0 JOIN a_t t1 USING (id) JOIN a_t t2 USING (id) JOIN a_t t3 USING (id)
  JOIN a_t t4 USING (id) JOIN a_t t5 USING (id) JOIN a_t t6 USING (id) JOIN a_t t7 USING (id)
  JOIN a_t t8 USING (id) JOIN a_t t9 USING (id) JOIN a_t t10 USING (id);
SELECT format('CREATE TABLE a_many_%s ()', g) FROM generate_series(1, 100) g \gexec

CREATE DATABASE a_db;
DROP DATABASE a_db;
SELECT 'DROP TABLE ' || string_agg(format('a_many_%s', g), ', ') FROM generate_series(1, 50) g \gexec

-- added in 1.1; a billion rows would take too long here, so that one is
-- granted by hand (before the end: a grant awards meta achievements quietly)
SELECT achievements.grant('t_all', 'billionaire');
BEGIN;
ROLLBACK;
INSERT INTO a_t VALUES (1) ON CONFLICT DO NOTHING;
INSERT INTO a_t VALUES (2) RETURNING id;
COMMENT ON TABLE a_t IS 'all of them';
MERGE INTO a_t USING a_t s ON a_t.id = s.id WHEN MATCHED THEN DO NOTHING;
BEGIN;
SAVEPOINT s;
ROLLBACK TO s;
COMMIT;
CREATE MATERIALIZED VIEW a_mv AS SELECT 1 AS id;
CREATE UNIQUE INDEX ON a_mv (id);
REFRESH MATERIALIZED VIEW CONCURRENTLY a_mv;
SELECT id FROM a_t FOR UPDATE SKIP LOCKED;
SELECT id, count(*) FROM a_t GROUP BY ROLLUP (id);
CREATE POLICY a_pol ON a_t USING (true);
NOTIFY a_chan;
-- 60 failing statements: one short line each
SET pg_achievements.notify = off;   -- see 04_errors
\set ECHO none
\set VERBOSITY terse
SELECT 'SELECT * FROM a_missing' FROM generate_series(1, 50) \gexec
SELECT 'SELECT 1 / 0' FROM generate_series(1, 10) \gexec
\set VERBOSITY default
\set ECHO all
RESET pg_achievements.notify;
SELECT count(*) FROM a_t, a_big;
SELECT count(*) FROM a_t WHERE id NOT IN (SELECT id FROM a_big);
BEGIN;
LOCK TABLE a_t;
COMMIT;
CREATE TABLE "a 🐘" (id int);
CREATE TABLE a_table_with_a_name_much_longer_than_sixty_three_bytes_believe_me (id int);
SET pg_achievements.test_now = '2027-01-01 00:00:10-08';
SELECT 'happy new year';
RESET pg_achievements.test_now;
ALTER SYSTEM SET pg_achievements.enabled = off;
ALTER SYSTEM RESET pg_achievements.enabled;
-- 30 days in a row; the last day completes every achievement
\set ECHO none
SELECT format('SET pg_achievements.test_now = %L',
              '2027-02-01 12:00:00-08'::timestamptz + g * interval '1 day'),
       'SELECT 1'
FROM generate_series(1, 30) g \gexec
\set ECHO all
RESET pg_achievements.test_now;

\o
\set QUIET off
SELECT achievements.sync();
SELECT count(*) AS unlocked, sum(points) AS score FROM achievements.my;
SELECT code FROM achievements.progress WHERE NOT unlocked;
