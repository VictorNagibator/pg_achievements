/* pg_achievements--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_achievements" to load this file. \quit

DO $$
BEGIN
    IF current_setting('pg_achievements.database', true) IS NULL THEN
        RAISE EXCEPTION 'pg_achievements must be loaded via shared_preload_libraries';
    END IF;
    IF current_database() <> current_setting('pg_achievements.database') THEN
        RAISE EXCEPTION 'pg_achievements must be created in database "%"',
            current_setting('pg_achievements.database')
            USING HINT = 'Change pg_achievements.database and restart the server to use another database.';
    END IF;
END
$$;

------------------------------------------------------------------------------
-- Tables
------------------------------------------------------------------------------

CREATE TYPE achievements.tier AS ENUM ('bronze', 'silver', 'gold', 'platinum');

CREATE TABLE achievements.catalog (
    code        text PRIMARY KEY,
    title       text NOT NULL,
    description text NOT NULL,
    tier        achievements.tier NOT NULL,
    points      int  NOT NULL,
    secret      bool NOT NULL DEFAULT false,
    counter_key text NOT NULL,
    threshold   bigint NOT NULL DEFAULT 1,
    enabled     bool NOT NULL DEFAULT true,
    builtin     bool NOT NULL DEFAULT true,
    CONSTRAINT catalog_code_check CHECK (code ~ '^[a-z][a-z0-9_]{0,62}$'),
    CONSTRAINT catalog_title_check CHECK (length(title) BETWEEN 1 AND 127),
    CONSTRAINT catalog_description_check CHECK (length(description) <= 255),
    CONSTRAINT catalog_points_check CHECK (points >= 0),
    CONSTRAINT catalog_threshold_check CHECK (threshold >= 1)
);

CREATE TABLE achievements.counters (
    role_name   name   NOT NULL,
    counter_key text   NOT NULL,
    value       bigint NOT NULL DEFAULT 0,
    updated_at  timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (role_name, counter_key)
);

CREATE TABLE achievements.awarded (
    role_name   name NOT NULL,
    code        text NOT NULL REFERENCES achievements.catalog(code) ON DELETE CASCADE,
    awarded_at  timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (role_name, code)
);

-- progress survives pg_dump / restore; builtin rows come from this script
SELECT pg_catalog.pg_extension_config_dump('achievements.catalog', 'WHERE NOT builtin');
SELECT pg_catalog.pg_extension_config_dump('achievements.counters', '');
SELECT pg_catalog.pg_extension_config_dump('achievements.awarded', '');

REVOKE ALL ON achievements.catalog, achievements.counters, achievements.awarded FROM PUBLIC;

------------------------------------------------------------------------------
-- Functions implemented in C
------------------------------------------------------------------------------

CREATE FUNCTION achievements.counter(key text)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pga_sql_counter'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION achievements.counter(text) IS
'Current value of a counter of the current role, including unflushed events';

CREATE FUNCTION achievements.sync(timeout_ms int DEFAULT 5000)
RETURNS bool
AS 'MODULE_PATHNAME', 'pga_sql_sync'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION achievements.sync(int) IS
'Waits until the worker has written everything counted so far to the tables';

CREATE FUNCTION achievements._reset(role name)
RETURNS void
AS 'MODULE_PATHNAME', 'pga_sql_reset'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION achievements._grant(role name, code text)
RETURNS bool
AS 'MODULE_PATHNAME', 'pga_sql_grant'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION achievements._counter_keys(OUT key text, OUT kind text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pga_sql_counter_keys'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION achievements._library_version()
RETURNS text
AS 'MODULE_PATHNAME', 'pga_sql_library_version'
LANGUAGE C STRICT IMMUTABLE;

REVOKE ALL ON FUNCTION achievements._grant(name, text) FROM PUBLIC;

------------------------------------------------------------------------------
-- SQL functions
------------------------------------------------------------------------------

-- Version of the builtin catalog, bumped by migrations that change it.
CREATE FUNCTION achievements._catalog_version()
RETURNS text
LANGUAGE sql IMMUTABLE
AS $$ SELECT '1.0'::text $$;

-- Did a role opt out with ALTER ROLE ... SET pg_achievements.opt_out = on?
CREATE FUNCTION achievements._opted_out(role name)
RETURNS bool
LANGUAGE sql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
    SELECT EXISTS (
        SELECT 1
        FROM pg_db_role_setting s
        JOIN pg_roles r ON r.oid = s.setrole
        CROSS JOIN LATERAL unnest(s.setconfig) AS cfg
        WHERE r.rolname = role
          AND (s.setdatabase = 0 OR s.setdatabase =
               (SELECT oid FROM pg_database WHERE datname = current_database()))
          AND lower(split_part(cfg, '=', 1)) = 'pg_achievements.opt_out'
          AND lower(split_part(cfg, '=', 2)) IN ('on', 'true', 'yes', '1', 't', 'y')
    )
$$;

CREATE FUNCTION achievements.version()
RETURNS text
LANGUAGE sql STABLE
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
    SELECT format('pg_achievements %s (library %s), catalog %s, %s achievements',
                  (SELECT extversion FROM pg_extension WHERE extname = 'pg_achievements'),
                  achievements._library_version(),
                  achievements._catalog_version(),
                  (SELECT count(*) FROM achievements.catalog WHERE enabled))
$$;

-- Resets counters and awards of a role.  A role may reset itself; resetting
-- others needs a superuser.  The worker removes the rows from the tables.
CREATE FUNCTION achievements.reset(role name DEFAULT current_user)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
    PERFORM achievements._reset(role);
    PERFORM achievements.sync();
END
$$;

-- Removes all data of a role that no longer exists.
CREATE FUNCTION achievements.forget_role(role name)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = role) THEN
        RAISE EXCEPTION 'role "%" still exists', role
            USING HINT = 'Use achievements.reset() for existing roles.';
    END IF;
    PERFORM achievements._reset(role);
    DELETE FROM achievements.counters WHERE role_name = role;
    DELETE FROM achievements.awarded WHERE role_name = role;
END
$$;

-- Adds a custom achievement based on an existing counter.
CREATE FUNCTION achievements.define(code text, title text, description text,
                                    tier achievements.tier, counter_key text,
                                    threshold bigint DEFAULT 1)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM achievements._counter_keys() k WHERE k.key = counter_key)
       AND counter_key NOT IN ('meta:all', 'meta:tier:bronze', 'meta:tier:silver',
                               'meta:tier:gold', 'meta:tier:platinum') THEN
        RAISE EXCEPTION 'unknown counter "%"', counter_key
            USING HINT = 'See SELECT * FROM achievements._counter_keys().';
    END IF;

    INSERT INTO achievements.catalog
        (code, title, description, tier, points, counter_key, threshold, builtin)
    VALUES
        (code, title, description, tier,
         CASE tier WHEN 'bronze' THEN 10 WHEN 'silver' THEN 25
                   WHEN 'gold' THEN 50 ELSE 100 END,
         counter_key, threshold, false);
END
$$;

-- Awards an achievement by hand.
CREATE FUNCTION achievements.grant(role name, code text)
RETURNS bool
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    granted bool;
BEGIN
    IF NOT EXISTS (SELECT 1 FROM achievements.catalog c WHERE c.code = grant.code) THEN
        RAISE EXCEPTION 'unknown achievement "%"', code;
    END IF;
    PERFORM achievements.sync();
    granted := achievements._grant(role, code);
    PERFORM achievements.sync();
    RETURN granted;
END
$$;

REVOKE ALL ON FUNCTION achievements.forget_role(name) FROM PUBLIC;
REVOKE ALL ON FUNCTION achievements.define(text, text, text, achievements.tier, text, bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION achievements.grant(name, text) FROM PUBLIC;

------------------------------------------------------------------------------
-- Views (owned by the extension owner, so they can read the tables)
------------------------------------------------------------------------------

CREATE VIEW achievements.my AS
SELECT c.tier, c.title, c.description, c.points, a.awarded_at, c.code
FROM achievements.awarded a
JOIN achievements.catalog c ON c.code = a.code
WHERE a.role_name = current_user
ORDER BY a.awarded_at, c.code;

CREATE VIEW achievements.progress AS
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
WHERE c.enabled
ORDER BY c.tier, c.code;

CREATE VIEW achievements.leaderboard AS
SELECT rank() OVER (ORDER BY sum(c.points) DESC) AS rank,
       a.role_name,
       sum(c.points)::bigint AS score,
       count(*) AS achievements,
       count(*) FILTER (WHERE c.tier = 'bronze') AS bronze,
       count(*) FILTER (WHERE c.tier = 'silver') AS silver,
       count(*) FILTER (WHERE c.tier = 'gold') AS gold,
       count(*) FILTER (WHERE c.tier = 'platinum') AS platinum
FROM achievements.awarded a
JOIN achievements.catalog c ON c.code = a.code
WHERE NOT achievements._opted_out(a.role_name)
GROUP BY a.role_name
ORDER BY score DESC, a.role_name;

CREATE VIEW achievements.catalog_public AS
SELECT c.code,
       c.title,
       CASE WHEN c.secret THEN '???' ELSE c.description END AS description,
       c.tier,
       c.points,
       c.secret
FROM achievements.catalog c
WHERE c.enabled
ORDER BY c.tier, c.code;

GRANT USAGE ON SCHEMA achievements TO PUBLIC;
GRANT SELECT ON achievements.my, achievements.progress,
                achievements.leaderboard, achievements.catalog_public TO PUBLIC;
