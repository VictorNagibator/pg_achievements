-- Counters detected at parse analysis and committed at ExecutorEnd.
SELECT pga_test_role('t_analyze');
CREATE TABLE t1 (id int PRIMARY KEY, a int, j jsonb);
CREATE TABLE t2 (id int PRIMARY KEY, t1_id int);
INSERT INTO t1 SELECT g, g, jsonb_build_object('k', g) FROM generate_series(1, 10) g;
INSERT INTO t2 SELECT g, g FROM generate_series(1, 10) g;
GRANT ALL ON t1, t2 TO t_analyze;
SET ROLE t_analyze;
\set QUIET on

-- stmt_select
SELECT 1 AS one;

-- select_star, also with a qualifier
SELECT * FROM t1 WHERE id = 1;
SELECT t1.* FROM t1 WHERE id = 1;
SELECT "t1".* FROM t1 WHERE id = 1;
-- not a star: column list, count(*), multiplication
SELECT id, a FROM t1 WHERE id = 1;
SELECT count(*) FROM t1;
SELECT a * 2 FROM t1 WHERE id = 1;

-- cte_any, cte_recursive
WITH x AS (SELECT 1 AS v) SELECT v FROM x;
WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 3)
SELECT count(*) FROM r;

-- window_func
SELECT id, row_number() OVER (ORDER BY id) FROM t1 WHERE id < 3;

-- lateral
SELECT t1.id, s.x FROM t1, LATERAL (SELECT t1.a + 1 AS x) s WHERE t1.id = 1;

-- jsonb_ops: four operators in one query, then one more
SELECT j -> 'k', j ->> 'k', j @> '{"k": 1}', j ? 'k' FROM t1 WHERE id = 1;
SELECT j -> 0 FROM t1 WHERE id = 1;

-- join_count_max keeps the maximum
SELECT count(*) FROM t1 JOIN t2 ON t2.t1_id = t1.id;
SELECT count(*) FROM t1 a JOIN t2 b ON b.t1_id = a.id JOIN t1 c ON c.id = b.id;
SELECT count(*) FROM t1 JOIN t2 ON t2.t1_id = t1.id;

-- null_eq: counted even though the query returns nothing
SELECT id FROM t1 WHERE a = NULL;

-- update_no_where / delete_no_where: counted at analysis, even on failure
UPDATE t1 SET a = a;
BEGIN;
DELETE FROM t2;
ROLLBACK;
UPDATE t1 SET a = 1 / 0;
-- with WHERE: nothing
UPDATE t1 SET a = a WHERE id = 1;

-- terminate_backend (pid -1 does not exist, the call still succeeds)
SELECT pg_terminate_backend(-1);

-- a failed query does not count stmt_select
SELECT 1 / 0;

-- night_query: 03:00-05:00 in the session time zone
SET pg_achievements.test_now = '2026-11-18 03:30:00-08';
SELECT 2 AS night;
-- SET and RESET run "at night" too and count as night queries
SET timezone = 'Asia/Novosibirsk';  -- 18:30 there: not night
SELECT 3 AS day;
RESET timezone;
RESET pg_achievements.test_now;

\set QUIET off
SELECT * FROM pga_counters();
RESET ROLE;
