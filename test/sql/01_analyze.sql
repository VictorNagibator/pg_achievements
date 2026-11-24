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
-- inside a CTE too (one more of each)
WITH u AS (UPDATE t1 SET a = a RETURNING id) SELECT count(*) FROM u;
BEGIN;
WITH d AS (DELETE FROM t2 RETURNING id) SELECT count(*) FROM d;
ROLLBACK;

-- terminate_backend (pid -1 does not exist, the call still succeeds)
SELECT pg_terminate_backend(-1);

-- upsert and returning; DELETE ... RETURNING * is not a SELECT *
INSERT INTO t1 VALUES (1, 1) ON CONFLICT (id) DO NOTHING;
INSERT INTO t1 VALUES (11, 11) ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a RETURNING id;
DELETE FROM t1 WHERE id = 11 RETURNING *;

-- merge
MERGE INTO t2 USING t1 ON t2.id = t1.id WHEN NOT MATCHED THEN DO NOTHING;

-- skip_locked: NOWAIT does not count
SELECT id FROM t1 WHERE id = 1 FOR UPDATE SKIP LOCKED;
SELECT id FROM t1 WHERE id = 1 FOR UPDATE NOWAIT;

-- grouping_sets
SELECT a, count(*) FROM t1 WHERE id = 1 GROUP BY CUBE (a) ORDER BY a;

-- cartesian_product: tables joined without any condition
SELECT count(*) FROM t1, t2;
SELECT count(*) FROM t1 CROSS JOIN t2;
-- with a condition, or with a function instead of a second table: nothing
SELECT count(*) FROM t1, t2 WHERE t2.t1_id = t1.id;
SELECT count(*) FROM t1, generate_series(1, 2);

-- not_in_subquery: only NOT IN with a subquery
SELECT count(*) FROM t1 WHERE id NOT IN (SELECT t1_id FROM t2);
SELECT count(*) FROM t1 WHERE id NOT IN (1, 2);
SELECT count(*) FROM t1 WHERE id IN (SELECT t1_id FROM t2);

-- listen_notify via the function
SELECT pg_notify('t_analyze', 'hi');

-- a failed query does not count stmt_select
SELECT 1 / 0;

-- night_query: 03:00-05:00 in the session time zone
SET pg_achievements.test_now = '2026-11-18 03:30:00-08';
SELECT 2 AS night;
-- SET and RESET run "at night" too and count as night queries
SET timezone = 'Asia/Novosibirsk';  -- 18:30 there: not night
SELECT 3 AS day;
RESET timezone;

-- streak_days: active days in a row, a gap starts a new run; the first
-- day is the configured test_now (2026-11-18), which is no run with these
SET pg_achievements.test_now = '2026-12-30 12:00:00-08';
SELECT 'day 1' AS streak;
SELECT 'day 1 again' AS streak;
SET pg_achievements.test_now = '2026-12-31 12:00:00-08';
SELECT 'day 2' AS streak;
-- new_year_query: the first minute of the year, local time
-- (the SET right after it also runs in that minute and counts too)
SET pg_achievements.test_now = '2027-01-01 00:00:30-08';
SELECT 'day 3, happy new year' AS streak;
SET pg_achievements.test_now = '2027-01-01 00:01:00-08';
SELECT 'one minute late' AS streak;
SET pg_achievements.test_now = '2027-01-03 12:00:00-08';
SELECT 'a gap: day 1 again' AS streak;
-- back in time: ignored
RESET pg_achievements.test_now;
SELECT 'the past' AS streak;
-- streak_state packs the last active day and the current run
SELECT to_char(to_date((achievements.counter('streak_state') >> 32)::text, 'J'), 'YYYY-MM-DD') AS last_day,
       achievements.counter('streak_state') & 4294967295 AS run,
       achievements.counter('streak_days') AS longest;

\set QUIET off
SELECT * FROM pga_counters();
RESET ROLE;
