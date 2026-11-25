-- Views and privileges.
SELECT achievements.sync();

-- an unprivileged role sees its own data through the views only
SELECT pga_test_role('t_view');
SET ROLE t_view;
SELECT * FROM achievements.counters;
SELECT * FROM achievements.awarded;
UPDATE achievements.catalog SET points = 1000;
SELECT count(*) FROM achievements.my;
RESET ROLE;

-- my: t_ddl holds a secret achievement (yolo)
SET ROLE t_ddl;
SELECT tier, title, points, code FROM achievements.my ORDER BY tier, code;
-- progress: secret descriptions are hidden until unlocked
SELECT code, description, threshold, current, unlocked
FROM achievements.progress
WHERE code IN ('yolo', 'time_machine', 'first_table', 'table_collector', 'gold_rush')
ORDER BY code;
RESET ROLE;

-- catalog_public hides every secret description
SELECT code, title, description, tier, points
FROM achievements.catalog_public WHERE secret ORDER BY code;

-- leaderboard, then opt-out of t_err
SELECT role_name, score, achievements, bronze, silver, gold, platinum
FROM achievements.leaderboard WHERE role_name LIKE 't\_%' ORDER BY role_name;
ALTER ROLE t_err SET pg_achievements.opt_out = on;
SELECT role_name FROM achievements.leaderboard WHERE role_name LIKE 't\_%' ORDER BY role_name;
ALTER ROLE t_err RESET pg_achievements.opt_out;

-- Counters this server can never bump (MERGE on PostgreSQL 14): their
-- achievements are left out of the views and define() refuses them.
-- 05_views_1.out holds the PostgreSQL 14 output.
SELECT achievements._counter_unsupported('merge') AS merge,
       achievements._counter_unsupported('connect') AS connect,
       achievements._counter_unsupported('meta:all') AS meta,
       achievements._counter_unsupported('no_such_counter') AS unknown;
SELECT (SELECT count(*) FROM achievements.progress WHERE code = 'merge_conflict') AS progress,
       (SELECT count(*) FROM achievements.catalog_public WHERE code = 'merge_conflict') AS catalog_public,
       (SELECT count(*) FROM achievements.rarity WHERE code = 'merge_conflict') AS rarity;
-- (no ROLLBACK here: that would unlock Ctrl+Z and shift later notices)
SELECT achievements.define('my_merge', 'My Merge', 'Merge twice.', 'bronze', 'merge', 2);
DELETE FROM achievements.catalog WHERE code = 'my_merge';
