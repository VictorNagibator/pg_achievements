-- Counters of errors (emit_log_hook).
SELECT pga_test_role('t_err');
SET ROLE t_err;
\set QUIET on
-- Achievements caused by errors are reported while the transaction aborts,
-- right after the ERROR; psql may print the two in either order.  Keep the
-- output stable by checking the awards in the view instead.
SET pg_achievements.notify = off;

-- err_syntax
SELEC 1;
SELECT FROM WHERE;

-- other errors do not count
SELECT 1 / 0;

-- deadlock and serialization failure, raised by hand; real ones are
-- covered by the TAP tests
DO $$ BEGIN RAISE EXCEPTION 'fake deadlock' USING ERRCODE = 'deadlock_detected'; END $$;
DO $$ BEGIN RAISE EXCEPTION 'fake conflict' USING ERRCODE = 'serialization_failure'; END $$;

-- an error caught inside PL/pgSQL is never reported, so it does not count
DO $$ BEGIN
    RAISE EXCEPTION 'caught' USING ERRCODE = 'deadlock_detected';
EXCEPTION WHEN OTHERS THEN NULL;
END $$;

RESET pg_achievements.notify;
\set QUIET off
SELECT * FROM pga_counters();
SELECT achievements.sync();
SELECT code FROM achievements.my ORDER BY code;
RESET ROLE;
