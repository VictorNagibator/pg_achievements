/* pg_achievements--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_achievements UPDATE TO '1.1'" to load this file. \quit

-- 1.1 changes the builtin catalog:
--  * platinum now holds a single achievement, "collect everything", worth
--    300 points; the other platinum achievements move to gold (50 points);
--  * 22 new achievements on the counters added by library 1.1;
--  * achievements.define() refuses the platinum tier;
--  * triggers merge rows restored by pg_dump with rows the worker has
--    already written, instead of failing on the primary key.
-- Awards are kept as they are, including platinum_collection earned before
-- the new achievements existed.

-- a custom achievement must not hold the code of a new builtin one
DO $$
DECLARE
    taken text;
BEGIN
    SELECT string_agg(code, ', ' ORDER BY code) INTO taken
    FROM achievements.catalog
    WHERE code IN (
        'ctrl_z', 'upsert', 'returning_hero', 'self_documenting',
        'merge_conflict', 'save_point', 'materialist', 'queue_master',
        'cube_master', 'big_brother', 'are_you_listening',
        'where_is_my_table', 'to_infinity', 'cartesian_explosion',
        'not_in_trouble', 'lock_hoarder', 'unicode_artist', 'novelist',
        'happy_new_query', 'regret', 'streak', 'billionaire');
    IF taken IS NOT NULL THEN
        RAISE EXCEPTION 'custom achievements use codes of new builtin achievements: %', taken
            USING HINT = 'Delete them from achievements.catalog (their awards go with them) and retry.';
    END IF;
END
$$;

CREATE OR REPLACE FUNCTION achievements._catalog_version()
RETURNS text
LANGUAGE sql IMMUTABLE
AS $$ SELECT '1.1'::text $$;

-- Adds a custom achievement based on an existing counter.  The platinum
-- tier holds the builtin "collect everything" achievement only.
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

    INSERT INTO achievements.catalog
        (code, title, description, tier, points, counter_key, threshold, builtin)
    VALUES
        (code, title, description, tier,
         CASE tier WHEN 'bronze' THEN 10 WHEN 'silver' THEN 25 ELSE 50 END,
         counter_key, threshold, false);
END
$$;

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

UPDATE achievements.catalog
SET tier = 'gold', points = 50
WHERE builtin AND code IN (
    'yolo', 'time_machine', 'ten_joins', 'scorched_earth', 'marathon',
    'serial_killer', 'gold_rush');

UPDATE achievements.catalog
SET title = 'Slonik''s Chosen', points = 300
WHERE builtin AND code = 'platinum_collection';

INSERT INTO achievements.catalog
    (code, title, description, tier, points, secret, counter_key, threshold)
VALUES
-- bronze
('ctrl_z',               'Ctrl+Z',                  'Roll back a transaction with ROLLBACK.',                    'bronze',   10,  false,  'rollback',                              1),
('upsert',               'Upsert Happens',          'Run INSERT ... ON CONFLICT.',                               'bronze',   10,  false,  'upsert',                                1),
('returning_hero',       'Returning Hero',          'Use RETURNING in a data-modifying statement.',              'bronze',   10,  false,  'returning',                             1),
('self_documenting',     'Self-Documenting',        'Describe an object with COMMENT ON.',                       'bronze',   10,  false,  'comment_on',                            1),
-- silver
('merge_conflict',       'Merge Conflict',          'Run a MERGE statement.',                                    'silver',   25,  false,  'merge',                                 1),
('save_point',           'Save Point',              'Roll back to a savepoint.',                                 'silver',   25,  false,  'rollback_to_savepoint',                 1),
('materialist',          'Materialist',             'Refresh a materialized view CONCURRENTLY.',                 'silver',   25,  false,  'refresh_matview_concurrently',          1),
('queue_master',         'Queue Master',            'Lock rows with SKIP LOCKED.',                               'silver',   25,  false,  'skip_locked',                           1),
('cube_master',          'Cube Master',             'Use GROUPING SETS, ROLLUP or CUBE.',                        'silver',   25,  false,  'grouping_sets',                         1),
('big_brother',          'Big Brother',             'Create a row-level security policy.',                       'silver',   25,  false,  'create_policy',                         1),
('are_you_listening',    'Are You Listening?',      'Use LISTEN, NOTIFY or pg_notify().',                        'silver',   25,  false,  'listen_notify',                         1),
-- gold
('where_is_my_table',    'Where Is My Table?',      'Query a table that does not exist 50 times.',               'gold',     50,  false,  'err_undefined_table',                  50),
('to_infinity',          'To Infinity and Beyond',  'Divide by zero 10 times.',                                  'gold',     50,  false,  'err_division_by_zero',                 10),
('cartesian_explosion',  'Cartesian Explosion',     'Join tables without any join condition.',                   'gold',     50,  false,  'cartesian_product',                     1),
('not_in_trouble',       'NOT IN Trouble',          'Use NOT IN with a subquery (mind the NULLs).',              'gold',     50,  false,  'not_in_subquery',                       1),
('lock_hoarder',         'Lock Hoarder',            'Take an ACCESS EXCLUSIVE lock with LOCK TABLE.',            'gold',     50,  false,  'lock_exclusive',                        1),
('unicode_artist',       'Unicode Artist',          'Create a table with an emoji in its name.',                 'gold',     50,  true,   'emoji_name',                            1),
('novelist',             'Novelist',                'Name a table so long that PostgreSQL truncates the name.',  'gold',     50,  true,   'long_name',                             1),
('happy_new_query',      'Happy New Query',         'Run a query in the first minute of a new year.',            'gold',     50,  true,   'new_year_query',                        1),
('regret',               'Regret',                  'Try to switch achievements off with ALTER SYSTEM.',         'gold',     50,  true,   'disable_attempt',                       1),
('streak',               'On a Roll',               'Run queries 30 days in a row.',                             'gold',     50,  false,  'streak_days',                          30),
('billionaire',          'Billionaire',             'Process a billion rows.',                                   'gold',     50,  false,  'rows_processed',               1000000000);
