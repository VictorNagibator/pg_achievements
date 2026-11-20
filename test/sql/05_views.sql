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
