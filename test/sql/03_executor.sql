-- Counters measured by the executor and at transaction end.
SELECT pga_test_role('t_exec');
CREATE TABLE big (id int);
INSERT INTO big SELECT generate_series(1, 2000);   -- big_table_rows = 1000
CREATE TABLE small (id int PRIMARY KEY);
INSERT INTO small SELECT generate_series(1, 10);
ANALYZE big, small;
GRANT SELECT ON big, small TO t_exec;
SET ROLE t_exec;
\set QUIET on

-- seqscan_big: only a sequential scan of a big table counts
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM small;
SELECT count(*) FROM big;
RESET enable_indexscan;
RESET enable_bitmapscan;

-- query_over_60s / query_over_1h: durations are shifted in the test build
SET pg_achievements.test_time_shift = '61s';
SELECT 1 AS slow;
SET pg_achievements.test_time_shift = '3601s';
SELECT 1 AS very_slow;

-- rollback_long_xact: only ROLLBACK (or abort) of an old transaction
BEGIN;
SELECT 1;
COMMIT;
BEGIN;
SELECT 1;
ROLLBACK;
RESET pg_achievements.test_time_shift;
BEGIN;
ROLLBACK;

\set QUIET off
SELECT * FROM pga_counters();
RESET ROLE;
