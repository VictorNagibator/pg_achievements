-- Installation and basic sanity checks.
-- The connecting superuser earns a few achievements here (Welcome etc.).
CREATE EXTENSION pg_achievements;
SELECT achievements.sync();

SELECT achievements.version();

-- 35 counters, 46 achievements: 12 bronze, 14 silver, 12 gold, 8 platinum
SELECT count(*) AS counters FROM achievements._counter_keys();
SELECT tier, count(*), min(points), max(points)
FROM achievements.catalog GROUP BY tier ORDER BY tier;

-- every builtin achievement refers to a known counter or a meta key
SELECT code, counter_key FROM achievements.catalog
WHERE counter_key NOT IN (SELECT key FROM achievements._counter_keys())
ORDER BY code;

-- the extension refuses to be created in another database
CREATE DATABASE pga_other;
\c pga_other
\set VERBOSITY terse
CREATE EXTENSION pg_achievements;
\set VERBOSITY default
\c contrib_regression
SET pg_achievements.notify = off;
DROP DATABASE pga_other;
RESET pg_achievements.notify;

-- helper for the other tests: a fresh role that collects events
CREATE FUNCTION pga_test_role(name text, superuser bool DEFAULT false)
RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE format('CREATE ROLE %I %s', name, CASE WHEN superuser THEN 'SUPERUSER' ELSE '' END);
    EXECUTE format('GRANT ALL ON SCHEMA public TO %I', name);
END $$;

-- counters of the current role, as a compact list
CREATE FUNCTION pga_counters()
RETURNS TABLE (key text, value bigint) LANGUAGE sql AS $$
    SELECT k.key, achievements.counter(k.key)
    FROM achievements._counter_keys() k
    WHERE achievements.counter(k.key) <> 0
    ORDER BY k.key
$$;
