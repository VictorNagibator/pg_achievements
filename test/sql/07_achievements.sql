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

-- platinum
CREATE DATABASE a_db;
DROP DATABASE a_db;
SELECT 'DROP TABLE ' || string_agg(format('a_many_%s', g), ', ') FROM generate_series(1, 50) g \gexec

\o
\set QUIET off
SELECT achievements.sync();
SELECT count(*) AS unlocked, sum(points) AS score FROM achievements.my;
SELECT code FROM achievements.progress WHERE NOT unlocked;
