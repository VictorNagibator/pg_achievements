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

------------------------------------------------------------------------------
-- Builtin catalog: 46 achievements
------------------------------------------------------------------------------

INSERT INTO achievements.catalog
    (code, title, description, tier, points, secret, counter_key, threshold)
VALUES
-- bronze: first steps
('hello_world',         'Hello, World',               'Run your first SELECT.',                                 'bronze',   10, false, 'stmt_select',        1),
('welcome',             'Welcome',                    'Connect to the server for the first time.',              'bronze',   10, false, 'connect',            1),
('first_table',         'First Table',                'Create your first table.',                               'bronze',   10, false, 'create_table',       1),
('first_index',         'Indexer',                    'Create your first index.',                               'bronze',   10, false, 'create_index',       1),
('first_function',      'Functionary',                'Create your first function or procedure.',               'bronze',   10, false, 'create_function',    1),
('first_cte',           'WITH Love',                  'Run a query with a common table expression.',            'bronze',   10, false, 'cte_any',            1),
('first_join',          'All Connected',              'Run a query with an explicit JOIN.',                     'bronze',   10, false, 'join_count_max',     1),
('first_explain',       'Investigator',               'Ask the planner with EXPLAIN.',                          'bronze',   10, false, 'explain_any',        1),
('typo_apprentice',     'Learning from Typos',        'Make 10 syntax errors.',                                 'bronze',   10, false, 'err_syntax',        10),
('star_gazer',          'Stargazer',                  'Run 100 queries with SELECT *.',                         'bronze',   10, false, 'select_star',      100),
('night_owl',           'Night Owl',                  'Run a query between 03:00 and 05:00.',                   'bronze',   10, false, 'night_query',        1),
('null_not_equal',      'NULL Is Not Equal to NULL',  'Compare with = NULL instead of IS NULL.',                'bronze',   10, false, 'null_eq',            1),
-- silver: mastering features
('detective',           'Detective',                  'Run EXPLAIN ANALYZE.',                                   'silver',   25, false, 'explain_analyze',    1),
('recursion',           'Recursion: See Recursion',   'Run a WITH RECURSIVE query.',                            'silver',   25, false, 'cte_recursive',      1),
('open_window',         'Open Window',                'Use a window function.',                                 'silver',   25, false, 'window_func',        1),
('lateral_thinking',    'Lateral Thinking',           'Use LATERAL.',                                           'silver',   25, false, 'lateral',            1),
('jsonb_juggler',       'JSONB Juggler',              'Use the JSONB operators ->, ->>, @> or ? 10 times.',     'silver',   25, false, 'jsonb_ops',         10),
('patient',             'Patient',                    'Wait for a query that runs longer than a minute.',       'silver',   25, false, 'query_over_60s',     1),
('no_locks',            'No Locks',                   'Create an index CONCURRENTLY.',                          'silver',   25, false, 'create_index_concurrently', 1),
('trigger_happy',       'Trigger Happy',              'Create a trigger.',                                      'silver',   25, false, 'create_trigger',     1),
('divide_and_conquer',  'Divide and Conquer',         'Create a partitioned table or a partition.',             'silver',   25, false, 'partition_create',   1),
('extender',            'Expanding Horizons',         'Create 3 extensions.',                                   'silver',   25, false, 'create_extension',   3),
('thousand_and_one',    'A Thousand and One Queries', 'Run 1001 SELECT statements.',                            'silver',   25, false, 'stmt_select',     1001),
('night_regular',       'Night Shift',                'Run 20 queries between 03:00 and 05:00.',                'silver',   25, false, 'night_query',       20),
('star_veteran',        'Star Veteran',               'Run 1000 queries with SELECT *.',                        'silver',   25, false, 'select_star',     1000),
('bronze_collector',    'Collector',                  'Collect every bronze achievement.',                      'silver',   25, false, 'meta:tier:bronze',   1),
-- gold: risky and rare events
('forgot_where',        'Forgot WHERE',               'UPDATE without a WHERE clause.',                         'gold',     50, false, 'update_no_where',    1),
('forgot_where_again',  'Forgot WHERE, Part 2',       'DELETE without a WHERE clause.',                         'gold',     50, false, 'delete_no_where',    1),
('mutual_understanding','Mutual Understanding',       'Take part in a deadlock.',                               'gold',     50, false, 'deadlock',           1),
('friday_deploy',       'Friday Deploy',              'Run DDL on a Friday after 18:00.',                       'gold',     50, false, 'ddl_friday_evening', 1),
('index_for_the_weak',  'Indexes Are for the Weak',   'Scan a table of a million rows sequentially.',           'gold',     50, false, 'seqscan_big',        1),
('executioner',         'Executioner',                'Call pg_terminate_backend.',                             'gold',     50, false, 'terminate_backend',  1),
('clean_slate',         'Clean Slate',                'TRUNCATE a table.',                                      'gold',     50, false, 'truncate',           1),
('table_owner',         'Master of the Table',        'Run VACUUM FULL.',                                       'gold',     50, false, 'vacuum_full',        1),
('serial_conflict',     'Conflict of Interest',       'Hit a serialization failure.',                           'gold',     50, false, 'serialization_failure', 1),
('many_joins',          'JOINs Without Borders',      'Run a query with 8 JOINs.',                              'gold',     50, false, 'join_count_max',     8),
('table_collector',     'Table Collector',            'Create 100 tables.',                                     'gold',     50, false, 'create_table',     100),
('deadlock_veteran',    'Deadlock Veteran',           'Take part in 5 deadlocks.',                              'gold',     50, false, 'deadlock',           5),
-- platinum: the stuff of legends
('yolo',                'You Only Live Once',         'Drop a database.',                                       'platinum',100, true,  'drop_database',      1),
('time_machine',        'Time Machine',               'Roll back a transaction that ran for over an hour.',     'platinum',100, true,  'rollback_long_xact', 1),
('ten_joins',           'Ten JOINs',                  'Run a query with 10 JOINs.',                             'platinum',100, false, 'join_count_max',    10),
('scorched_earth',      'Scorched Earth',             'Drop 50 tables.',                                        'platinum',100, false, 'drop_table',        50),
('marathon',            'Marathon',                   'Wait for a query that runs longer than an hour.',        'platinum',100, true,  'query_over_1h',      1),
('serial_killer',       'Serial Executioner',         'Call pg_terminate_backend 10 times.',                    'platinum',100, true,  'terminate_backend', 10),
('gold_rush',           'Gold Rush',                  'Collect every gold achievement.',                        'platinum',100, false, 'meta:tier:gold',     1),
('platinum_collection', 'Platinum Collection',        'Collect every other achievement.',                       'platinum',100, false, 'meta:all',           1);
