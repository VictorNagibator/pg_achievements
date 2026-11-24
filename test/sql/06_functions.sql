-- SQL API and settings.
SELECT pga_test_role('t_fn');
SELECT pga_test_role('t_other');
GRANT CREATE ON SCHEMA public TO t_fn;
SET ROLE t_fn;
SELECT 1 AS first;
SELECT achievements.counter('stmt_select');
SELECT achievements.counter('no_such_counter');

-- opt-out: nothing is counted
SET pg_achievements.opt_out = on;
SELECT 1;
SELECT 1;
RESET pg_achievements.opt_out;
SELECT achievements.counter('stmt_select');

-- nested statements are not counted unless track_nested (superuser only)
DO $$ BEGIN EXECUTE 'CREATE TABLE fn_nested (id int)'; END $$;
SELECT achievements.counter('create_table');
SET pg_achievements.track_nested = on;
RESET ROLE;
SET pg_achievements.track_nested = on;
SET ROLE t_fn;
DO $$ BEGIN EXECUTE 'DROP TABLE fn_nested'; EXECUTE 'CREATE TABLE fn_nested (id int)'; END $$;
SELECT achievements.counter('create_table');
RESET ROLE;
RESET pg_achievements.track_nested;
SET ROLE t_fn;

-- notify = off: the achievement is still awarded, silently
SET pg_achievements.notify = off;
WITH x AS (SELECT 1) SELECT * FROM x;
RESET pg_achievements.notify;
SELECT achievements.sync();
SELECT code FROM achievements.my ORDER BY code;

-- emoji style (the database and the client use UTF8)
SET pg_achievements.notice_style = emoji;
SELECT id, rank() OVER (ORDER BY id) FROM (VALUES (1)) v(id);
RESET pg_achievements.notice_style;

-- reset: own progress yes, someone else's no
SELECT achievements.reset('t_other');
-- the SELECT calling reset() is itself the first statement after the reset,
-- so Hello, World is earned again right away
SELECT achievements.reset();
SELECT achievements.sync();
SELECT code FROM achievements.my ORDER BY code;
SELECT achievements.counter('stmt_select'), achievements.counter('cte_any');

-- administrative functions are not for everyone
SELECT achievements.grant('t_fn', 'yolo');
SELECT achievements.define('x', 'X', 'X.', 'bronze', 'stmt_select', 1);
SELECT achievements.forget_role('t_fn');
RESET ROLE;

-- grant by hand (no notice: the granting session is not the receiver)
SELECT achievements.grant('t_fn', 'yolo');
SELECT achievements.grant('t_fn', 'yolo');
SELECT achievements.grant('t_fn', 'no_such_code');
SET ROLE t_fn;
SELECT code FROM achievements.my ORDER BY code;
RESET ROLE;

-- define a custom achievement on an existing counter
SELECT achievements.define('truncator', 'Truncator', 'TRUNCATE 2 tables.', 'silver', 'truncate', 2);
SELECT achievements.define('bad', 'Bad', 'Bad.', 'silver', 'no_such_counter', 2);
-- platinum is reserved for the builtin "collect everything" achievement
SELECT achievements.define('shiny', 'Shiny', 'Shiny.', 'platinum', 'truncate', 1);
SELECT achievements.sync();
SELECT code, points, builtin FROM achievements.catalog WHERE NOT builtin;
SET ROLE t_fn;
CREATE TABLE fn_t (id int);
TRUNCATE fn_t;
TRUNCATE fn_t;
RESET ROLE;
-- custom achievements do not change the meta ones
SELECT count(*) FROM achievements.catalog WHERE builtin;
DELETE FROM achievements.catalog WHERE code = 'truncator';
SELECT achievements.sync();
-- defined again, the deleted achievement starts unearned and can be won
SELECT achievements.define('truncator', 'Truncator', 'TRUNCATE 4 tables.', 'silver', 'truncate', 4);
SELECT achievements.sync();
SET ROLE t_fn;
SELECT unlocked FROM achievements.progress WHERE code = 'truncator';
TRUNCATE fn_t;
TRUNCATE fn_t;
SELECT achievements.sync();
SELECT code FROM achievements.my WHERE code = 'truncator';
RESET ROLE;
DELETE FROM achievements.catalog WHERE code = 'truncator';

-- texts of any length the table accepts are shown whole: 127 and 255
-- Cyrillic letters take 254 and 509 bytes (the full stop is ASCII)
SELECT achievements.define('long_ru', repeat('Ж', 127), repeat('Ы', 254) || '.',
                           'bronze', 'comment_on', 1);
SELECT achievements.sync();
SET ROLE t_fn;
COMMENT ON TABLE fn_t IS 'commented';
RESET ROLE;
-- the texts come from a UTF8 database: in a LATIN1 one, letters it cannot
-- show become "?" instead of garbage
SELECT achievements.define('ru_lock', 'Замок', 'Взять замок.', 'bronze', 'lock_exclusive', 1);
SELECT achievements.sync();
CREATE DATABASE pga_latin1 TEMPLATE template0 ENCODING 'LATIN1';
\c pga_latin1
SET client_encoding = 'UTF8';
CREATE TABLE fn_l (id int);
GRANT ALL ON fn_l TO t_fn;
SET ROLE t_fn;
BEGIN;
LOCK TABLE fn_l;
COMMIT;
\c contrib_regression
SET pg_achievements.notify = off;
DROP DATABASE pga_latin1;
RESET pg_achievements.notify;
DELETE FROM achievements.catalog WHERE code IN ('long_ru', 'ru_lock');

-- forget_role refuses existing roles, then cleans up a dropped one
SELECT achievements.forget_role('t_other');
SELECT pga_test_role('t_gone');
SET ROLE t_gone;
SELECT 1 AS soon_gone;
RESET ROLE;
SELECT achievements.sync();
DROP OWNED BY t_gone;
DROP ROLE t_gone;
SELECT count(*) FROM achievements.counters WHERE role_name = 't_gone';
SELECT achievements.forget_role('t_gone');
SELECT count(*) FROM achievements.counters WHERE role_name = 't_gone';
SELECT count(*) FROM achievements.awarded WHERE role_name = 't_gone';

-- Rows restored by pg_dump while the server runs meet the rows the worker
-- has written: they are merged (sums add up, maxima and the earlier award
-- time win), and the worker keeps adding to them instead of overwriting.
SELECT pga_test_role('t_merge');
SET ROLE t_merge;
\set QUIET on
SET pg_achievements.notify = off;
SELECT 1 FROM generate_series(1, 3);
SELECT achievements.sync();
\set QUIET off
RESET ROLE;
-- the last sync() of t_merge counts once it has finished: write it too
SELECT achievements.sync();
SELECT counter_key, value FROM achievements.counters
WHERE role_name = 't_merge' AND counter_key IN ('stmt_select', 'rows_processed')
ORDER BY counter_key;
-- what COPY of a dump does: plain inserts, some of them for existing keys
INSERT INTO achievements.counters (role_name, counter_key, value) VALUES
    ('t_merge', 'stmt_select', 100),
    ('t_merge', 'rows_processed', 1000),
    ('t_merge', 'join_count_max', 5),
    ('t_merge', 'cte_any', 7);
INSERT INTO achievements.awarded (role_name, code, awarded_at) VALUES
    ('t_merge', 'hello_world', '2000-01-01 00:00:00+00');
SET ROLE t_merge;
\set QUIET on
-- one more SELECT with a CTE and a JOIN (join_count_max 1 < 5)
WITH x AS (SELECT 1 AS id) SELECT count(*) FROM x JOIN x y USING (id);
SELECT achievements.sync();
RESET pg_achievements.notify;
\set QUIET off
RESET ROLE;
SELECT achievements.sync();
SELECT counter_key, value FROM achievements.counters
WHERE role_name = 't_merge'
  AND counter_key IN ('stmt_select', 'rows_processed', 'join_count_max', 'cte_any')
ORDER BY counter_key;
SELECT code, awarded_at AT TIME ZONE 'UTC' AS awarded_at_utc
FROM achievements.awarded WHERE role_name = 't_merge' AND code = 'hello_world';
