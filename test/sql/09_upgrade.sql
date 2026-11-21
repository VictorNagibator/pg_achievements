-- ALTER EXTENSION UPDATE from 1.0 gives the same catalog and functions as a
-- fresh 1.1 install, and keeps the progress made under 1.0.
SET pg_achievements.notify = off;
-- tier as text: a column of the extension's type would block DROP EXTENSION
CREATE TABLE fresh_catalog AS
    SELECT code, title, description, tier::text, points, secret, counter_key, threshold, enabled
    FROM achievements.catalog WHERE builtin;
CREATE TABLE fresh_functions AS
    SELECT p.proname, pg_get_functiondef(p.oid) AS def
    FROM pg_proc p WHERE p.pronamespace = 'achievements'::regnamespace;

DROP EXTENSION pg_achievements;
CREATE EXTENSION pg_achievements VERSION '1.0';
SELECT achievements.sync();
SELECT achievements.version();

-- progress under 1.0: a platinum achievement and the old meta one
SELECT pga_test_role('t_upgrade');
SELECT achievements.grant('t_upgrade', 'ten_joins');
SELECT achievements.grant('t_upgrade', 'platinum_collection');
-- a custom platinum achievement is allowed in 1.0 and survives the update
SELECT achievements.define('old_shiny', 'Old Shiny', 'Shiny.', 'platinum', 'truncate', 1);

ALTER EXTENSION pg_achievements UPDATE TO '1.1';
SELECT achievements.sync();
SELECT achievements.version();

-- the builtin catalog and the functions match a fresh install
(TABLE fresh_catalog
 EXCEPT SELECT code, title, description, tier::text, points, secret, counter_key, threshold, enabled
        FROM achievements.catalog WHERE builtin)
UNION ALL
(SELECT code, title, description, tier::text, points, secret, counter_key, threshold, enabled
 FROM achievements.catalog WHERE builtin
 EXCEPT TABLE fresh_catalog);
SELECT proname FROM (
    (TABLE fresh_functions
     EXCEPT SELECT p.proname, pg_get_functiondef(p.oid)
            FROM pg_proc p WHERE p.pronamespace = 'achievements'::regnamespace)
    UNION ALL
    (SELECT p.proname, pg_get_functiondef(p.oid)
     FROM pg_proc p WHERE p.pronamespace = 'achievements'::regnamespace
     EXCEPT TABLE fresh_functions)) d;

-- the merge triggers for restored rows exist after the update too
SELECT tgrelid::regclass AS "table", tgname FROM pg_trigger
WHERE tgrelid IN ('achievements.counters'::regclass, 'achievements.awarded'::regclass)
  AND NOT tgisinternal
ORDER BY 1, 2;

-- awards are kept, with the new tiers and points
SET ROLE t_upgrade;
SELECT code, tier, title, points FROM achievements.my ORDER BY code;
RESET ROLE;
SELECT code, tier, points FROM achievements.catalog WHERE NOT builtin;

-- the update refuses to run over a custom achievement with a new code
DROP EXTENSION pg_achievements;
CREATE EXTENSION pg_achievements VERSION '1.0';
SELECT achievements.define('streak', 'Mine', 'Mine.', 'bronze', 'connect', 1);
\set VERBOSITY terse
ALTER EXTENSION pg_achievements UPDATE TO '1.1';
\set VERBOSITY default
SELECT achievements.version();

-- without it the update goes through
DELETE FROM achievements.catalog WHERE code = 'streak';
ALTER EXTENSION pg_achievements UPDATE TO '1.1';
SELECT achievements.version();
