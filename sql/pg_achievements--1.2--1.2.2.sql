/* pg_achievements--1.2--1.2.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_achievements UPDATE TO '1.2.2'" to load this file. \quit

-- 1.2.2 adds PostgreSQL 14 and 15 support.  PostgreSQL 14 has no MERGE, so
-- an achievement on the "merge" counter can never be earned there:
--  * achievements._counter_unsupported() tells which counters this server
--    cannot bump;
--  * achievements.progress, catalog_public and rarity, and the count of
--    achievements.version(), leave such achievements out;
--  * achievements.define() refuses such a counter.
-- On PostgreSQL 15 and later nothing changes.  The catalog stays as it is.

CREATE FUNCTION achievements._counter_unsupported(key text)
RETURNS bool
AS 'MODULE_PATHNAME', 'pga_sql_counter_unsupported'
LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION achievements.version()
RETURNS text
LANGUAGE sql STABLE
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
    SELECT format('pg_achievements %s (library %s), catalog %s, %s achievements',
                  (SELECT extversion FROM pg_extension WHERE extname = 'pg_achievements'),
                  achievements._library_version(),
                  achievements._catalog_version(),
                  (SELECT count(*) FROM achievements.catalog c
                   WHERE c.enabled AND NOT achievements._counter_unsupported(c.counter_key)))
$$;

CREATE OR REPLACE FUNCTION achievements.define(code text, title text, description text,
                                    tier achievements.tier, counter_key text,
                                    threshold bigint DEFAULT 1)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
    IF tier = 'platinum' THEN
        RAISE EXCEPTION 'the platinum tier is reserved for the builtin achievement "platinum_collection"'
            USING HINT = 'Use the bronze, silver or gold tier.';
    END IF;

    IF NOT EXISTS (SELECT 1 FROM achievements._counter_keys() k WHERE k.key = counter_key)
       AND counter_key NOT IN ('meta:all', 'meta:tier:bronze', 'meta:tier:silver',
                               'meta:tier:gold', 'meta:tier:platinum') THEN
        RAISE EXCEPTION 'unknown counter "%"', counter_key
            USING HINT = 'See SELECT * FROM achievements._counter_keys().';
    END IF;

    IF achievements._counter_unsupported(counter_key) THEN
        RAISE EXCEPTION 'counter "%" is not supported by this PostgreSQL version', counter_key
            USING HINT = 'The "merge" counter needs PostgreSQL 15 or later.';
    END IF;

    INSERT INTO achievements.catalog
        (code, title, description, tier, points, counter_key, threshold, builtin)
    VALUES
        (code, title, description, tier,
         CASE tier WHEN 'bronze' THEN 10 WHEN 'silver' THEN 25 ELSE 50 END,
         counter_key, threshold, false);
END
$$;

CREATE OR REPLACE VIEW achievements.progress AS
SELECT c.code,
       c.tier,
       c.title,
       CASE WHEN c.secret AND a.code IS NULL THEN '???' ELSE c.description END AS description,
       c.points,
       CASE WHEN c.counter_key LIKE 'meta:%' THEN NULL ELSE c.threshold END AS threshold,
       CASE WHEN c.counter_key LIKE 'meta:%' THEN NULL
            ELSE achievements.counter(c.counter_key) END AS current,
       a.code IS NOT NULL AS unlocked,
       a.awarded_at
FROM achievements.catalog c
LEFT JOIN achievements.awarded a ON a.code = c.code AND a.role_name = current_user
WHERE c.enabled AND NOT achievements._counter_unsupported(c.counter_key)
ORDER BY c.tier, c.code;

CREATE OR REPLACE VIEW achievements.catalog_public AS
SELECT c.code,
       c.title,
       CASE WHEN c.secret THEN '???' ELSE c.description END AS description,
       c.tier,
       c.points,
       c.secret
FROM achievements.catalog c
WHERE c.enabled AND NOT achievements._counter_unsupported(c.counter_key)
ORDER BY c.tier, c.code;

CREATE OR REPLACE VIEW achievements.rarity AS
WITH players AS (
    SELECT p.role_name
    FROM (SELECT role_name FROM achievements.counters
          UNION
          SELECT role_name FROM achievements.awarded) p
    WHERE NOT achievements._opted_out(p.role_name)
),
total AS (
    SELECT count(*) AS n FROM players
)
SELECT c.code,
       c.title,
       c.tier,
       c.points,
       count(a.role_name) AS holders,
       t.n AS players,
       CASE WHEN t.n > 0 THEN round(100.0 * count(a.role_name) / t.n, 1) END AS percent
FROM achievements.catalog c
CROSS JOIN total t
LEFT JOIN achievements.awarded a
       ON a.code = c.code AND a.role_name IN (SELECT role_name FROM players)
WHERE c.enabled AND NOT achievements._counter_unsupported(c.counter_key)
GROUP BY c.code, c.title, c.tier, c.points, t.n
ORDER BY holders, c.tier DESC, c.code;
