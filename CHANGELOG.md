# Changelog

## 1.2.2 — 2026-11-25

PostgreSQL 14 and 15 support. Install the new version, restart the server
and run `ALTER EXTENSION pg_achievements UPDATE`.

- PostgreSQL 14 and 15 are supported (CI and Docker images `pg14`, `pg15`).
- PostgreSQL 14 has no `MERGE`, so `merge_conflict` cannot be earned there.
  On 14 it is left out of `achievements.progress`, `catalog_public`,
  `rarity` and `achievements.version()`, `platinum_collection` does not
  require it, and `achievements.define()` refuses the `merge` counter.
  On 15 and later nothing changes.

## 1.2.1 — 2026-11-24

Bug fixes. Only the library changes: install it and restart the server,
no `ALTER EXTENSION` is needed.

- Statements prepared with the extended protocol (pgbench `-M prepared`,
  pgx, asyncpg, JDBC) were not counted at all, and neither were their days
  for `streak_days`: every execution now counts. The extension turns query
  ids on for that, as `pg_stat_statements` does. `PREPARE` and `EXECUTE`
  count like the statement they prepare or run.
- A typo such as `EXPLAIN (ANALYZE maybe)`, `VACUUM (FULL maybe)`, a name
  with too many dots or a cross-database name in `DROP TABLE` was reported
  as an internal error and switched tracking off for the session.
- Long titles and descriptions of custom achievements were cut, possibly in
  the middle of a character, which could switch tracking off for clients
  with another encoding. In databases with another encoding than
  `pg_achievements.database` the texts came out garbled; non-ASCII
  characters are now shown as `?` there.
- A custom achievement deleted and defined again could never be earned
  again by roles that had it before, and deleted achievements still counted
  in the score of notices.
- After `ALTER ROLE ... RENAME` the sessions of the role went on counting
  under the old name.
- After `pg_terminate_backend()` of the launcher, the flush worker was never
  restarted again until the server restarted. The launcher now comes back,
  and only one flush worker ever runs.
- `UPDATE` and `DELETE` without `WHERE` inside a CTE now count.
  `CREATE TABLE ... AS` and `CREATE INDEX` with `IF NOT EXISTS` on an
  existing object no longer count.

## 1.2.0 — 2026-11-21

Achievements become social.

- New awards are announced with `NOTIFY` on the channel
  `pg_achievements.notify_channel` (`achievements` by default, empty to turn
  it off), with a JSON payload: role, code, title, tier, points and time.
  Opted-out roles are not announced.
- New view `achievements.rarity`: how many players hold each achievement,
  like the "% of players" of game consoles.
- The notice tells how rare a fresh achievement is: "Unlocked by 3 of 14
  players (21%)." or "You are the first of 14 players to unlock it."
- PostgreSQL 19 is supported.
- A Docker image to try it out: `ghcr.io/victornagibator/pg_achievements`,
  tagged `pg16` to `pg19`.
- Upgrade with `ALTER EXTENSION pg_achievements UPDATE` after installing the
  new version and restarting the server. The catalog does not change.

## 1.1.0 — 2026-11-21

Platinum is now a single trophy, as on game consoles: collect everything
else to get it.

- Platinum holds one achievement, `platinum_collection`, renamed to
  Slonik's Chosen and worth 300 points. Unlocking it shows Slonik, the
  PostgreSQL elephant, and a congratulation in the notice.
- The other seven platinum achievements (`yolo`, `time_machine`,
  `ten_joins`, `scorched_earth`, `marathon`, `serial_killer`, `gold_rush`)
  move to gold and are worth 50 points.
- 22 new achievements, 68 in all (16 bronze, 21 silver, 30 gold,
  1 platinum): Ctrl+Z, Upsert Happens, Returning Hero, Self-Documenting,
  Merge Conflict, Save Point, Materialist, Queue Master, Cube Master,
  Big Brother, Are You Listening?, Where Is My Table?, To Infinity and
  Beyond, Cartesian Explosion, NOT IN Trouble, Lock Hoarder, On a Roll,
  Billionaire and four secret ones.
- 23 new counters, 58 in all, among them `streak_days` (consecutive active
  days) and `rows_processed`.
- `achievements.define()` no longer accepts the platinum tier. Custom
  platinum achievements created with 1.0 are kept.
- Upgrade with `ALTER EXTENSION pg_achievements UPDATE` after installing the
  new version and restarting the server. Awards and counters are kept.

Fixed:

- Restoring a `pg_dump` into a running server could fail with a duplicate
  key error, when the worker wrote counters or awards of the restoring
  session between `CREATE EXTENSION` and the `COPY` of the dump, and the
  worker could overwrite restored counters with its own values. The worker
  now writes increments (or the new maximum for "max" counters), publishes
  its bookkeeping only after a commit, and locks the tables while writing;
  triggers merge restored rows with existing ones.

## 1.0.0 — 2026-11-21

First release.

- 46 builtin achievements in four tiers (12 bronze, 14 silver, 12 gold,
  8 platinum), three of them meta achievements.
- 35 per-role counters collected through parse analysis, utility, executor,
  error, transaction and authentication hooks.
- Instant `NOTICE` in the client, emoji or ASCII style.
- Views `achievements.my`, `progress`, `leaderboard`, `catalog_public`;
  functions `counter`, `sync`, `version`, `reset`, `forget_role`, `define`,
  `grant`.
- Background worker started by a launcher; progress survives restarts,
  crashes and `pg_dump`; nothing is written on a hot standby.
- PostgreSQL 16, 17 and 18.
