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
