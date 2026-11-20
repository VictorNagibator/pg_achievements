-- Counters of errors (emit_log_hook).
SELECT pga_test_role('t_err');
SET ROLE t_err;
\set QUIET on

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

\set QUIET off
SELECT * FROM pga_counters();
RESET ROLE;
