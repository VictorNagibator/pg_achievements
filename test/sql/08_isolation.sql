-- An internal error of the extension never breaks the user's query: it
-- prints one WARNING and disables tracking for the rest of the session.
SELECT pga_test_role('t_iso');
SET ROLE t_iso;
SET pg_achievements.test_inject_error = on;
SELECT 1 AS still_works;
-- no second warning, and nothing is counted any more
SELECT 2 AS still_works;
RESET pg_achievements.test_inject_error;
SELECT 3 AS not_counted;
SELECT achievements.counter('stmt_select');
RESET ROLE;

-- a new session is tracked again
\c
SET ROLE t_iso;
SELECT 4 AS counted;
RESET ROLE;
