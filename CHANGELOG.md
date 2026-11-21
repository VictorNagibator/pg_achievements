# Changelog

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
