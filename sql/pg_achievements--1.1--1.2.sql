/* pg_achievements--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_achievements UPDATE TO '1.2'" to load this file. \quit

-- 1.2 keeps the builtin catalog and adds:
--  * a NOTIFY announcement of every new award (pg_achievements.notify_channel);
--  * the achievements.rarity view.
-- The 1.2 library also adds the rarity line to the notice, which needs no
-- SQL changes.

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
WHERE c.enabled
GROUP BY c.code, c.title, c.tier, c.points, t.n
ORDER BY holders, c.tier DESC, c.code;

GRANT SELECT ON achievements.rarity TO PUBLIC;
