/* pg_achievements--1.2.2.sql */

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

-- Achievements on such a counter can never be earned on this server (MERGE
-- on PostgreSQL 14): the views leave them out.  Not IMMUTABLE: the answer
-- changes with the server version, e.g. after pg_upgrade.
CREATE FUNCTION achievements._counter_unsupported(key text)
RETURNS bool
AS 'MODULE_PATHNAME', 'pga_sql_counter_unsupported'
LANGUAGE C STRICT STABLE PARALLEL SAFE;

REVOKE ALL ON FUNCTION achievements._grant(name, text) FROM PUBLIC;

------------------------------------------------------------------------------
-- SQL functions
------------------------------------------------------------------------------

-- Version of the builtin catalog, bumped by migrations that change it.
CREATE FUNCTION achievements._catalog_version()
RETURNS text
LANGUAGE sql IMMUTABLE
AS $$ SELECT '1.1'::text $$;

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
                  (SELECT count(*) FROM achievements.catalog c
                   WHERE c.enabled AND NOT achievements._counter_unsupported(c.counter_key)))
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

-- Adds a custom achievement based on an existing counter.  The platinum
-- tier holds the builtin "collect everything" achievement only.
CREATE FUNCTION achievements.define(code text, title text, description text,
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

-- Awards an achievement by hand.
CREATE FUNCTION achievements.grant(role name, code text)
RETURNS bool
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    granted bool;
BEGIN
    -- "grant" is a reserved word, so the parameter cannot be qualified
    IF NOT EXISTS (SELECT 1 FROM achievements.catalog c WHERE c.code = $2) THEN
        RAISE EXCEPTION 'unknown achievement "%"', $2;
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
-- Merging restored rows
------------------------------------------------------------------------------

-- pg_dump stores counters and awards as plain COPY data.  When a dump is
-- restored into a running server, the worker may already have written rows
-- for the same keys (the restoring session counts events too), and COPY
-- would fail on the primary key.  These triggers merge such a row into the
-- existing one instead: sum counters add up, max counters keep the larger
-- value, an award keeps the earlier time.  The worker writes its own rows
-- with ON CONFLICT and the same rules, so the triggers change nothing for it.

CREATE FUNCTION achievements._merge_counter()
RETURNS trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
    UPDATE achievements.counters c
    SET value = CASE WHEN EXISTS (SELECT 1 FROM achievements._counter_keys() k
                                  WHERE k.key = NEW.counter_key AND k.kind = 'max')
                     THEN greatest(c.value, NEW.value)
                     ELSE c.value + NEW.value END,
        updated_at = greatest(c.updated_at, NEW.updated_at)
    WHERE c.role_name = NEW.role_name AND c.counter_key = NEW.counter_key;
    IF FOUND THEN
        RETURN NULL;
    END IF;
    RETURN NEW;
END
$$;

CREATE FUNCTION achievements._merge_award()
RETURNS trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
    UPDATE achievements.awarded a
    SET awarded_at = least(a.awarded_at, NEW.awarded_at)
    WHERE a.role_name = NEW.role_name AND a.code = NEW.code;
    IF FOUND THEN
        RETURN NULL;
    END IF;
    RETURN NEW;
END
$$;

REVOKE ALL ON FUNCTION achievements._merge_counter(), achievements._merge_award() FROM PUBLIC;

CREATE TRIGGER merge_existing BEFORE INSERT ON achievements.counters
    FOR EACH ROW EXECUTE FUNCTION achievements._merge_counter();
CREATE TRIGGER merge_existing BEFORE INSERT ON achievements.awarded
    FOR EACH ROW EXECUTE FUNCTION achievements._merge_award();

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
WHERE c.enabled AND NOT achievements._counter_unsupported(c.counter_key)
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
WHERE c.enabled AND NOT achievements._counter_unsupported(c.counter_key)
ORDER BY c.tier, c.code;

GRANT USAGE ON SCHEMA achievements TO PUBLIC;
GRANT SELECT ON achievements.my, achievements.progress,
                achievements.leaderboard, achievements.catalog_public TO PUBLIC;

------------------------------------------------------------------------------
-- Announcements and rarity (1.2)
------------------------------------------------------------------------------

-- Every new award is announced with NOTIFY on the channel named by
-- pg_achievements.notify_channel (empty: no announcements), so a chat bot
-- or a dashboard can LISTEN instead of polling the tables.  The payload is
-- JSON without the description, which stays hidden for secret achievements:
--   {"role": "alice", "code": "forgot_where", "title": "Forgot WHERE",
--    "tier": "gold", "points": 50, "awarded_at": "..."}
-- The trigger fires only for rows really inserted: a row merged by
-- _merge_award() or skipped by ON CONFLICT is not a new award.  Roles that
-- opted out are not announced, as they are not on the leaderboard.
-- NOTIFY is delivered at commit, and only to sessions connected to
-- pg_achievements.database.

CREATE FUNCTION achievements._announce_award()
RETURNS trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    channel text := current_setting('pg_achievements.notify_channel', true);
    payload text;
BEGIN
    IF coalesce(channel, '') = '' OR achievements._opted_out(NEW.role_name) THEN
        RETURN NULL;
    END IF;
    SELECT json_build_object('role', NEW.role_name,
                             'code', c.code,
                             'title', c.title,
                             'tier', c.tier,
                             'points', c.points,
                             'awarded_at', NEW.awarded_at)::text
    INTO payload
    FROM achievements.catalog c
    WHERE c.code = NEW.code;
    IF payload IS NOT NULL THEN
        PERFORM pg_notify(channel, payload);
    END IF;
    RETURN NULL;
END
$$;

REVOKE ALL ON FUNCTION achievements._announce_award() FROM PUBLIC;

CREATE TRIGGER announce AFTER INSERT ON achievements.awarded
    FOR EACH ROW EXECUTE FUNCTION achievements._announce_award();

-- How rare each achievement is, like the "% of players" of game consoles.
-- A player is a role with any saved progress that did not opt out.
CREATE VIEW achievements.rarity AS
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

GRANT SELECT ON achievements.rarity TO PUBLIC;

------------------------------------------------------------------------------
-- Builtin catalog: 68 achievements
------------------------------------------------------------------------------

INSERT INTO achievements.catalog
    (code, title, description, tier, points, secret, counter_key, threshold)
VALUES
-- bronze: first steps
('hello_world',           'Hello, World',                'Run your first SELECT.',                                    'bronze',     10,  false,  'stmt_select',                           1),
('welcome',               'Welcome',                     'Connect to the server for the first time.',                 'bronze',     10,  false,  'connect',                               1),
('first_table',           'First Table',                 'Create your first table.',                                  'bronze',     10,  false,  'create_table',                          1),
('first_index',           'Indexer',                     'Create your first index.',                                  'bronze',     10,  false,  'create_index',                          1),
('first_function',        'Functionary',                 'Create your first function or procedure.',                  'bronze',     10,  false,  'create_function',                       1),
('first_cte',             'WITH Love',                   'Run a query with a common table expression.',               'bronze',     10,  false,  'cte_any',                               1),
('first_join',            'All Connected',               'Run a query with an explicit JOIN.',                        'bronze',     10,  false,  'join_count_max',                        1),
('first_explain',         'Investigator',                'Ask the planner with EXPLAIN.',                             'bronze',     10,  false,  'explain_any',                           1),
('typo_apprentice',       'Learning from Typos',         'Make 10 syntax errors.',                                    'bronze',     10,  false,  'err_syntax',                           10),
('star_gazer',            'Stargazer',                   'Run 100 queries with SELECT *.',                            'bronze',     10,  false,  'select_star',                         100),
('night_owl',             'Night Owl',                   'Run a query between 03:00 and 05:00.',                      'bronze',     10,  false,  'night_query',                           1),
('null_not_equal',        'NULL Is Not Equal to NULL',   'Compare with = NULL instead of IS NULL.',                   'bronze',     10,  false,  'null_eq',                               1),
('ctrl_z',                'Ctrl+Z',                      'Roll back a transaction with ROLLBACK.',                    'bronze',     10,  false,  'rollback',                              1),
('upsert',                'Upsert Happens',              'Run INSERT ... ON CONFLICT.',                               'bronze',     10,  false,  'upsert',                                1),
('returning_hero',        'Returning Hero',              'Use RETURNING in a data-modifying statement.',              'bronze',     10,  false,  'returning',                             1),
('self_documenting',      'Self-Documenting',            'Describe an object with COMMENT ON.',                       'bronze',     10,  false,  'comment_on',                            1),
-- silver: mastering features
('detective',             'Detective',                   'Run EXPLAIN ANALYZE.',                                      'silver',     25,  false,  'explain_analyze',                       1),
('recursion',             'Recursion: See Recursion',    'Run a WITH RECURSIVE query.',                               'silver',     25,  false,  'cte_recursive',                         1),
('open_window',           'Open Window',                 'Use a window function.',                                    'silver',     25,  false,  'window_func',                           1),
('lateral_thinking',      'Lateral Thinking',            'Use LATERAL.',                                              'silver',     25,  false,  'lateral',                               1),
('jsonb_juggler',         'JSONB Juggler',               'Use the JSONB operators ->, ->>, @> or ? 10 times.',        'silver',     25,  false,  'jsonb_ops',                            10),
('patient',               'Patient',                     'Wait for a query that runs longer than a minute.',          'silver',     25,  false,  'query_over_60s',                        1),
('no_locks',              'No Locks',                    'Create an index CONCURRENTLY.',                             'silver',     25,  false,  'create_index_concurrently',             1),
('trigger_happy',         'Trigger Happy',               'Create a trigger.',                                         'silver',     25,  false,  'create_trigger',                        1),
('divide_and_conquer',    'Divide and Conquer',          'Create a partitioned table or a partition.',                'silver',     25,  false,  'partition_create',                      1),
('extender',              'Expanding Horizons',          'Create 3 extensions.',                                      'silver',     25,  false,  'create_extension',                      3),
('thousand_and_one',      'A Thousand and One Queries',  'Run 1001 SELECT statements.',                               'silver',     25,  false,  'stmt_select',                        1001),
('night_regular',         'Night Shift',                 'Run 20 queries between 03:00 and 05:00.',                   'silver',     25,  false,  'night_query',                          20),
('star_veteran',          'Star Veteran',                'Run 1000 queries with SELECT *.',                           'silver',     25,  false,  'select_star',                        1000),
('merge_conflict',        'Merge Conflict',              'Run a MERGE statement.',                                    'silver',     25,  false,  'merge',                                 1),
('save_point',            'Save Point',                  'Roll back to a savepoint.',                                 'silver',     25,  false,  'rollback_to_savepoint',                 1),
('materialist',           'Materialist',                 'Refresh a materialized view CONCURRENTLY.',                 'silver',     25,  false,  'refresh_matview_concurrently',          1),
('queue_master',          'Queue Master',                'Lock rows with SKIP LOCKED.',                               'silver',     25,  false,  'skip_locked',                           1),
('cube_master',           'Cube Master',                 'Use GROUPING SETS, ROLLUP or CUBE.',                        'silver',     25,  false,  'grouping_sets',                         1),
('big_brother',           'Big Brother',                 'Create a row-level security policy.',                       'silver',     25,  false,  'create_policy',                         1),
('are_you_listening',     'Are You Listening?',          'Use LISTEN, NOTIFY or pg_notify().',                        'silver',     25,  false,  'listen_notify',                         1),
('bronze_collector',      'Collector',                   'Collect every bronze achievement.',                         'silver',     25,  false,  'meta:tier:bronze',                      1),
-- gold: risky, rare and legendary events
('forgot_where',          'Forgot WHERE',                'UPDATE without a WHERE clause.',                            'gold',       50,  false,  'update_no_where',                       1),
('forgot_where_again',    'Forgot WHERE, Part 2',        'DELETE without a WHERE clause.',                            'gold',       50,  false,  'delete_no_where',                       1),
('mutual_understanding',  'Mutual Understanding',        'Take part in a deadlock.',                                  'gold',       50,  false,  'deadlock',                              1),
('friday_deploy',         'Friday Deploy',               'Run DDL on a Friday after 18:00.',                          'gold',       50,  false,  'ddl_friday_evening',                    1),
('index_for_the_weak',    'Indexes Are for the Weak',    'Scan a table of a million rows sequentially.',              'gold',       50,  false,  'seqscan_big',                           1),
('executioner',           'Executioner',                 'Call pg_terminate_backend.',                                'gold',       50,  false,  'terminate_backend',                     1),
('clean_slate',           'Clean Slate',                 'TRUNCATE a table.',                                         'gold',       50,  false,  'truncate',                              1),
('table_owner',           'Master of the Table',         'Run VACUUM FULL.',                                          'gold',       50,  false,  'vacuum_full',                           1),
('serial_conflict',       'Conflict of Interest',        'Hit a serialization failure.',                              'gold',       50,  false,  'serialization_failure',                 1),
('many_joins',            'JOINs Without Borders',       'Run a query with 8 JOINs.',                                 'gold',       50,  false,  'join_count_max',                        8),
('table_collector',       'Table Collector',             'Create 100 tables.',                                        'gold',       50,  false,  'create_table',                        100),
('deadlock_veteran',      'Deadlock Veteran',            'Take part in 5 deadlocks.',                                 'gold',       50,  false,  'deadlock',                              5),
('yolo',                  'You Only Live Once',          'Drop a database.',                                          'gold',       50,  true,   'drop_database',                         1),
('time_machine',          'Time Machine',                'Roll back a transaction that ran for over an hour.',        'gold',       50,  true,   'rollback_long_xact',                    1),
('ten_joins',             'Ten JOINs',                   'Run a query with 10 JOINs.',                                'gold',       50,  false,  'join_count_max',                       10),
('scorched_earth',        'Scorched Earth',              'Drop 50 tables.',                                           'gold',       50,  false,  'drop_table',                           50),
('marathon',              'Marathon',                    'Wait for a query that runs longer than an hour.',           'gold',       50,  true,   'query_over_1h',                         1),
('serial_killer',         'Serial Executioner',          'Call pg_terminate_backend 10 times.',                       'gold',       50,  true,   'terminate_backend',                    10),
('where_is_my_table',     'Where Is My Table?',          'Query a table that does not exist 50 times.',               'gold',       50,  false,  'err_undefined_table',                  50),
('to_infinity',           'To Infinity and Beyond',      'Divide by zero 10 times.',                                  'gold',       50,  false,  'err_division_by_zero',                 10),
('cartesian_explosion',   'Cartesian Explosion',         'Join tables without any join condition.',                   'gold',       50,  false,  'cartesian_product',                     1),
('not_in_trouble',        'NOT IN Trouble',              'Use NOT IN with a subquery (mind the NULLs).',              'gold',       50,  false,  'not_in_subquery',                       1),
('lock_hoarder',          'Lock Hoarder',                'Take an ACCESS EXCLUSIVE lock with LOCK TABLE.',            'gold',       50,  false,  'lock_exclusive',                        1),
('unicode_artist',        'Unicode Artist',              'Create a table with an emoji in its name.',                 'gold',       50,  true,   'emoji_name',                            1),
('novelist',              'Novelist',                    'Name a table so long that PostgreSQL truncates the name.',  'gold',       50,  true,   'long_name',                             1),
('happy_new_query',       'Happy New Query',             'Run a query in the first minute of a new year.',            'gold',       50,  true,   'new_year_query',                        1),
('regret',                'Regret',                      'Try to switch achievements off with ALTER SYSTEM.',         'gold',       50,  true,   'disable_attempt',                       1),
('streak',                'On a Roll',                   'Run queries 30 days in a row.',                             'gold',       50,  false,  'streak_days',                          30),
('billionaire',           'Billionaire',                 'Process a billion rows.',                                   'gold',       50,  false,  'rows_processed',               1000000000),
('gold_rush',             'Gold Rush',                   'Collect every gold achievement.',                           'gold',       50,  false,  'meta:tier:gold',                        1),
-- platinum: one to rule them all
('platinum_collection',   'Slonik''s Chosen',            'Collect every other achievement.',                          'platinum',  300,  false,  'meta:all',                              1);
