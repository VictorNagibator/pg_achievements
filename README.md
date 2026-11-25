# pg_achievements

[![CI](https://github.com/VictorNagibator/pg_achievements/actions/workflows/ci.yml/badge.svg)](https://github.com/VictorNagibator/pg_achievements/actions/workflows/ci.yml)
[![PostgreSQL 14 – 19](https://img.shields.io/badge/PostgreSQL-14%20%E2%80%93%2019-336791?logo=postgresql&logoColor=white)](#requirements)
[![PGXN version](https://badge.fury.io/pg/pg_achievements.svg)](https://pgxn.org/dist/pg_achievements/)
[![pgext.cloud](https://img.shields.io/badge/pgext.cloud-pg__achievements-336791)](https://pgext.cloud/ext/pg_achievements)
[![License: PostgreSQL](https://img.shields.io/badge/license-PostgreSQL-blue)](LICENSE)

Game achievements for PostgreSQL users. The extension watches what roles do in
the database and hands out bronze, silver and gold achievements; like a
trophy on a game console, the single platinum one goes to those who have
collected all the others. The notice arrives right in the client, in the
same session:

```text
postgres=> UPDATE accounts SET balance = 0;
NOTICE:  🥇 [GOLD] Achievement unlocked: Forgot WHERE (+50)
DETAIL:  UPDATE without a WHERE clause. Total score: 135.
Unlocked by 3 of 14 players (21%).
UPDATE 1042
```

Some achievements nudge you toward PostgreSQL features (`EXPLAIN ANALYZE`,
recursive CTEs, window functions, partitioning, `CREATE INDEX CONCURRENTLY`).
Others gently mock classic mistakes, like the `UPDATE` above or a Friday
evening deploy. All 68 builtin achievements are listed in
[docs/ACHIEVEMENTS.md](docs/ACHIEVEMENTS.md).

Collect them all and Slonik, the PostgreSQL elephant, comes to congratulate
you in person:

```text
NOTICE:  💎 [PLATINUM] Achievement unlocked: Slonik's Chosen (+300)
DETAIL:  Collect every other achievement. Total score: 2485.

            _.-""""-._
          .'          '.
     .--./  (o)    (o)  \.--.
    (    |              |    )
     '--'\      __      /'--'
          '.   |  |   .'
            '-.|  |.-'
               |  |
               |  |
               '--'

🎉 C O N G R A T U L A T I O N S 🎉
You have collected all 68 achievements.
Slonik bows to you: there is nothing left to unlock.
HINT:  Go outside, touch grass, run VACUUM.
```

## Requirements

- PostgreSQL 14 to 19 on Linux; the main development version is 18
  (on 14, which has no `MERGE`, see [Known limitations](#known-limitations));
- the server must preload the library (`shared_preload_libraries`), so managed
  services such as RDS or Cloud SQL are not supported;
- server headers and PGXS: `postgresql-server-dev-NN` on Debian/Ubuntu,
  plus `libkrb5-dev` (the server headers include the GSSAPI header when
  PostgreSQL is built with GSSAPI, as the PGDG packages are).

There are no other dependencies.

## Try it in Docker

A PostgreSQL 18 server with the extension preloaded and created in the
`postgres` database:

```sh
docker run -d --name pga -e POSTGRES_PASSWORD=secret ghcr.io/victornagibator/pg_achievements
docker exec -it pga psql -U postgres
```

Other PostgreSQL versions are tagged `pg14` to `pg19`, for example
`ghcr.io/victornagibator/pg_achievements:pg17`. To build the image from a
checkout: `docker build -t pg_achievements .` (add
`--build-arg PG_MAJOR=17` for another version).

## Installation

From [PGXN](https://pgxn.org/dist/pg_achievements/):

```sh
pgxn install pg_achievements
```

Binary packages are listed in the [pgext.cloud](https://pgext.cloud/ext/pg_achievements)
extension catalog, together with the install commands for each
distribution and PostgreSQL version.

Or from source:

```sh
make                 # uses the first pg_config in PATH
sudo make install    # or: make PG_CONFIG=/path/to/pg_config install
```

Add the library to `postgresql.conf` and restart the server:

```ini
shared_preload_libraries = 'pg_achievements'   # keep other libraries in the list
#pg_achievements.database = 'postgres'         # database that stores the tables
```

Then create the extension in that database:

```sql
CREATE EXTENSION pg_achievements;
```

The extension can only be created in `pg_achievements.database`, but events
are collected from every database of the cluster. If that database does not
exist yet, the background worker waits until it is created.

### Upgrading from 1.2.0 or 1.2.1

Install the new version, restart the server and update the SQL objects:

```sql
ALTER EXTENSION pg_achievements UPDATE;
```

1.2.2 adds PostgreSQL 14 and 15 support. On PostgreSQL 15 and later the
update changes nothing visible; on 14 it hides `merge_conflict` from the
views (see [Known limitations](#known-limitations)). Until the update the
1.2.2 library works with the 1.2 SQL objects.

### Upgrading from 1.1

Install the new version, restart the server and update the SQL objects:

```sql
ALTER EXTENSION pg_achievements UPDATE;
```

1.2 keeps the catalog and the progress; it adds the NOTIFY announcements
and the `achievements.rarity` view. Until the update the 1.2 library works
with the 1.1 catalog, just without them.

### Upgrading from 1.0

Install the new version, restart the server (the preloaded library changes
its shared memory), then update the SQL objects:

```sql
ALTER EXTENSION pg_achievements UPDATE;
```

Until the update the 1.1 library keeps working with the 1.0 catalog.
Progress and awards are kept. The former platinum achievements become gold
(50 points instead of 100), and the only platinum one left is
`platinum_collection`, renamed to Slonik's Chosen and worth 300 points. A
role that already holds it keeps it, even without the new achievements. The
update stops with an error if a custom achievement uses the code of a new
builtin one: delete it from `achievements.catalog` (with its awards) first.

## Usage

```sql
-- my achievements
SELECT tier, title, points, awarded_at FROM achievements.my;

-- everything with my progress; secret descriptions stay hidden until unlocked
SELECT code, title, current, threshold, unlocked FROM achievements.progress;

-- leaderboard of all roles that did not opt out
SELECT * FROM achievements.leaderboard;

-- how many players hold each achievement, rarest first
SELECT code, tier, holders, players, percent FROM achievements.rarity;

-- the catalog without hidden fields
SELECT * FROM achievements.catalog_public;

-- one counter of the current role, including events not flushed yet
SELECT achievements.counter('stmt_select');

-- wait until the worker has written everything to the tables
SELECT achievements.sync();
```

Every new award is announced with `NOTIFY`, so a chat bot or a dashboard
can listen instead of polling the tables. Connect to
`pg_achievements.database` and listen on the channel `achievements`:

```text
postgres=> LISTEN achievements;
Asynchronous notification "achievements" with payload "{"role" : "alice",
"code" : "forgot_where", "title" : "Forgot WHERE", "tier" : "gold",
"points" : 50, "awarded_at" : "2026-11-21T18:03:12.345+00:00"}" received
from server process with PID 4242.
```

The payload has no description, so secret achievements stay secret. Awards
of opted-out roles are not announced. Set `pg_achievements.notify_channel`
to another channel name, or to an empty string to turn this off.

Opt out of tracking for a role, or only for a session:

```sql
ALTER ROLE robot SET pg_achievements.opt_out = on;
SET pg_achievements.opt_out = on;
```

Quiet mode, or medals without emoji:

```sql
SET pg_achievements.notify = off;          -- still awarded, just no notice
SET pg_achievements.notice_style = ascii;  -- [GOLD] instead of 🥇 [GOLD]
```

### Functions

| Function | Who | What it does |
| --- | --- | --- |
| `achievements.counter(key)` | any role | Current value of a counter of the current role |
| `achievements.sync(timeout_ms DEFAULT 5000)` | any role | Flushes local events and waits for the worker to write them; returns `false` on timeout |
| `achievements.version()` | any role | Extension, library and catalog versions |
| `achievements.reset(role DEFAULT current_user)` | the role itself or a superuser | Removes counters and achievements of the role |
| `achievements.forget_role(role)` | superuser | Removes the data of a role that no longer exists (`DROP ROLE` keeps it) |
| `achievements.define(code, title, description, tier, counter_key, threshold)` | superuser | Adds a custom bronze, silver or gold achievement on an existing counter; points follow the tier (10/25/50) |
| `achievements.grant(role, code)` | superuser | Awards an achievement by hand |

A custom achievement becomes active within `flush_interval_ms` after the
transaction that defined it commits. Counter keys are listed by
`SELECT * FROM achievements._counter_keys()`.

Example:

```sql
SELECT achievements.define('vacuum_fan', 'Vacuum Fan', 'Run VACUUM FULL 10 times.',
                           'gold', 'vacuum_full', 10);
```

### Settings

| Parameter | Context | Default | Meaning |
| --- | --- | --- | --- |
| `pg_achievements.enabled` | reload | `on` | Turns all tracking on or off |
| `pg_achievements.database` | restart | `postgres` | Database that stores the tables |
| `pg_achievements.opt_out` | user | `off` | The role or session is not tracked at all |
| `pg_achievements.notify` | user | `on` | Show notices in the session |
| `pg_achievements.notice_style` | user | `emoji` | `emoji` (needs UTF8 on both sides) or `ascii` |
| `pg_achievements.notify_channel` | reload | `achievements` | `NOTIFY` channel of new awards; empty turns it off |
| `pg_achievements.track_nested` | superuser | `off` | Also count statements run inside functions |
| `pg_achievements.flush_interval_ms` | reload | `1000` | How often counters go to the tables (min 100) |
| `pg_achievements.big_table_rows` | reload | `1000000` | Row estimate (`reltuples`) of a "big" table for `seqscan_big` |
| `pg_achievements.max_roles` | restart | `1024` | Roles kept in shared memory |

## How it works

```text
 client backends                      shared memory                 worker
 ───────────────                      ─────────────                 ──────
 hooks bump local deltas  ──commit──▶ per-role counters (atomics) ──▶ achievements.counters
 threshold reached?                   award bits + award queue    ──▶ achievements.awarded
   └─ claim the bit, NOTICE now       catalog snapshot           ◀── achievements.catalog
```

- Hooks (`post_parse_analyze`, `ProcessUtility`, `ExecutorRun/Finish/End`,
  `emit_log_hook`, transaction callbacks and `ClientAuthentication_hook`) only
  increment counters in backend memory. No I/O, no SPI, no exclusive locks.
- At commit (and abort, backend exit, or at most every `flush_interval_ms`
  inside long transactions) the deltas are added to per-role atomics in shared
  memory.
- The backend compares the counters with the thresholds itself. When a
  threshold is reached it claims the award bit atomically, so 50 sessions of
  one role racing for the same achievement still produce exactly one award,
  and sends the notice right away.
- A background worker writes counters and awards to the tables once per
  `flush_interval_ms`, in its own transactions. Tracking therefore does not
  depend on the user's transaction: rolling back does not take an achievement
  back. After a crash at most `flush_interval_ms` of progress is lost.
- The worker writes what a counter gained since its last write (or the new
  maximum), not an absolute value, so rows that reach the tables from
  elsewhere, such as a `pg_dump` being restored, are added to rather than
  overwritten.
- The worker is started by a small launcher, which restarts it 10 seconds
  after a failure. On a hot standby the worker never writes.

## Counters

Every achievement checks one counter. `sum` counters add up events, `max`
counters keep the largest value seen.

| Key | Type | Counts |
| --- | --- | --- |
| `connect` | sum | successful connections |
| `stmt_select` | sum | top-level `SELECT` statements that succeeded |
| `select_star` | sum | `SELECT *` / `SELECT t.*` |
| `err_syntax` | sum | syntax errors (`42601`) |
| `explain_any` / `explain_analyze` | sum | `EXPLAIN` / `EXPLAIN ANALYZE` |
| `cte_any` / `cte_recursive` | sum | queries with `WITH` / `WITH RECURSIVE` |
| `window_func` | sum | queries with window functions |
| `lateral` | sum | queries with `LATERAL` |
| `jsonb_ops` | sum | uses of the JSONB operators `->`, `->>`, `@>`, `?` |
| `join_count_max` | max | most explicit `JOIN`s in one query |
| `null_eq` | sum | `= NULL` instead of `IS NULL` |
| `update_no_where` / `delete_no_where` | sum | `UPDATE` / `DELETE` without `WHERE` |
| `seqscan_big` | sum | queries with a sequential scan of a big table |
| `query_over_60s` / `query_over_1h` | sum | queries longer than a minute / an hour |
| `deadlock` / `serialization_failure` | sum | errors `40P01` / `40001` |
| `rollback_long_xact` | sum | rollbacks of transactions older than an hour |
| `ddl_friday_evening` | sum | DDL on a Friday after 18:00 |
| `night_query` | sum | statements between 03:00 and 05:00 |
| `create_table`, `create_index`, `create_index_concurrently`, `create_function`, `create_trigger`, `create_extension`, `partition_create` | sum | the corresponding `CREATE` commands |
| `drop_table`, `drop_database`, `truncate`, `vacuum_full` | sum | the corresponding commands; `DROP TABLE` counts every table dropped |
| `terminate_backend` | sum | calls of `pg_terminate_backend` in queries that succeeded |
| `rollback` / `rollback_to_savepoint` | sum | `ROLLBACK` of a transaction block / `ROLLBACK TO SAVEPOINT` |
| `upsert` / `returning` / `merge` | sum | statements with `ON CONFLICT` / `RETURNING`, `MERGE` statements |
| `comment_on`, `create_policy`, `refresh_matview_concurrently` | sum | `COMMENT ON`, `CREATE POLICY`, `REFRESH MATERIALIZED VIEW CONCURRENTLY` |
| `skip_locked` / `grouping_sets` | sum | queries with `SKIP LOCKED` / `GROUPING SETS`, `ROLLUP` or `CUBE` |
| `listen_notify` | sum | `LISTEN`, `NOTIFY` and calls of `pg_notify()` |
| `err_undefined_table` / `err_division_by_zero` | sum | errors `42P01` / `22012` |
| `cartesian_product` | sum | queries joining plain tables with no condition at all (`FROM a, b` or `CROSS JOIN` without `WHERE`) |
| `not_in_subquery` | sum | queries with `NOT IN (SELECT ...)` |
| `lock_exclusive` | sum | `LOCK TABLE` in `ACCESS EXCLUSIVE` mode (the default) |
| `emoji_name` / `long_name` | sum | `CREATE TABLE` with an emoji in the name / with a name truncated to 63 bytes |
| `new_year_query` | sum | statements in the first minute of January 1 |
| `disable_attempt` | sum | `ALTER SYSTEM SET pg_achievements.enabled = off` |
| `streak_days` | max | longest run of consecutive days with at least one statement |
| `streak_state` | max | internal: last active day and the current run, packed into one number |
| `rows_processed` | sum | rows returned or changed by top-level statements |

Counting rules:

- Only top-level statements count unless `track_nested` is on. A statement
  run by a top-level `EXECUTE` counts like a top-level one.
- A prepared statement counts every time it runs. Its features are found
  again by its query id, so the extension turns query ids on (as
  `pg_stat_statements` does): with `compute_query_id = auto` they show up in
  `pg_stat_activity.query_id` and `EXPLAIN VERBOSE`.
- `update_no_where`, `delete_no_where`, `null_eq`, `select_star`,
  `cartesian_product` and `not_in_subquery` count at parse analysis, because
  the attempt is what matters. Everything else counts only when the
  statement finishes successfully.
- Statements that change nothing (`DROP TABLE IF EXISTS` on a missing table,
  `CREATE TABLE IF NOT EXISTS` or `CREATE EXTENSION IF NOT EXISTS` on an
  existing object) do not count.
- The role is `current_user` (`GetUserId()`), so after `SET ROLE` events go to
  the new role. Progress is kept by role name: after `ALTER ROLE ... RENAME`
  a role starts from scratch under its new name, and the old rows can be
  removed with `achievements.forget_role()`.
- Times of day and days (for `streak_days`) use the session `timezone`.
- Background processes (autovacuum, replication, the worker itself) are never
  tracked.

## Privacy and security

- Query texts, parameters and object names are never stored or logged. The
  extension keeps only counters and award facts, and its notices and warnings
  are sent with `errhidestmt`.
- The tables are not readable by `PUBLIC`. Ordinary roles use the views,
  which only show their own progress (the leaderboard shows scores of all
  roles that did not opt out).
- Any role that can connect to `pg_achievements.database` may `LISTEN` to
  the announcements, which carry the role name and the achievement, the
  same facts as the leaderboard. Set `pg_achievements.notify_channel = ''`
  if that is too much.
- Administrative functions are revoked from `PUBLIC`. Functions that act on
  the caller (`counter`, `sync`, `reset`) run as the caller and check
  permissions in C, because `SECURITY DEFINER` would hide the caller's
  identity. `version()` is `SECURITY DEFINER` with a fixed `search_path`.
- An internal error in a hook prints one `WARNING`, disables tracking for that
  session and never breaks the user's query.

## Known limitations

- Errors are seen through `emit_log_hook`, which runs only for messages that
  go to the server log: keep `log_min_messages` at `error` or lower
  (the default `warning` is fine).
- Achievements earned by an error (deadlock, syntax error) are reported while
  the transaction aborts, so psql may print the notice before or after the
  `ERROR` line.
- With `compute_query_id = off` a prepared statement counts only when it
  runs in the transaction that prepared it; drivers that prepare once and
  execute later (pgx, asyncpg, JDBC, `pgbench -M prepared`) are then barely
  counted. The attempt counters (`SELECT *`, `= NULL`, a missing `WHERE`, …)
  count once per `PREPARE` in any case.
- Achievement texts are stored in the encoding of `pg_achievements.database`.
  Notices in a database with another encoding show non-ASCII characters of
  custom achievements as `?`.
- PostgreSQL 14 has no `MERGE`, so `merge_conflict` cannot be earned there.
  On 14 it is left out of `achievements.progress`, `catalog_public`,
  `rarity` and the count of `achievements.version()`,
  `platinum_collection` does not require it, and `achievements.define()`
  refuses the `merge` counter. It comes back after `pg_upgrade` to a newer
  version.
- Client-side meta-commands such as `\d` are invisible; the server only sees
  the SQL they send.
- `pg_dump` saves progress and custom achievements. Restoring into a running
  server is safe: rows restored for a key the worker has already written are
  merged with it (sums add up, maxima and the earlier award time win). Then
  restart the server, so that shared memory, and with it the thresholds and
  notices, see the restored progress. Restored awards are announced with
  `NOTIFY` like new ones.
- The rarity line of a notice counts the roles active since the server
  started or loaded from the tables, including roles that opted out after
  playing; `achievements.rarity` leaves those out. The line is skipped
  while there is only one player.

## Testing

The test build adds GUCs that the tests rely on: `pg_achievements.test_now`
(fake clock), `pg_achievements.test_time_shift` (added to measured durations)
and `pg_achievements.test_inject_error` (simulates an internal failure).
They do not exist in a normal build:

```sh
make PGA_TESTING=1 && make PGA_TESTING=1 install
make PGA_TESTING=1 installcheck      # pg_regress + TAP (needs --enable-tap-tests)
```

`installcheck` starts its own temporary server, so no running instance is
needed. The regression tests also check that updating from 1.0 gives the
same catalog as a fresh install. The TAP tests cover restarts and crashes, `kill -9` of the worker,
50 sessions racing for one award, `pg_dump`/restore, `pg_stat_statements` in
both preload orders, hot standby and real deadlocks.

A few regression outputs differ on PostgreSQL 14 (no `MERGE`, another
`pg_terminate_backend` warning): `test/expected/*_1.out` hold the PostgreSQL
14 variants, and pg_regress accepts either file. When a change touches a
test that has such a file (`00_setup`, `01_analyze`, `05_views`,
`07_achievements`, `09_upgrade`), regenerate both.

## Performance

Every statement passes through the hooks, so tracking has a price. The worst
case is many tiny statements on a CPU-saturated server: `bench/run.sh`
measures it with pgbench, comparing a server **without the library** with
one that tracks (the baseline is not `enabled = off`, which still pays for
the hooks). All clients use the same role, so they share its counters.

PostgreSQL 18.7 built with `-O2`, scale 10, median of 5 runs of 15 s,
AMD Ryzen 5 5600 (12 vCPU) under WSL2:

| Case | Clients | Without, tps | With, tps | Cost |
| --- | ---: | ---: | ---: | ---: |
| select-only (`-S`) | 1 | 8368 | 8040 | 3.9% |
| select-only (`-S`) | 16 | 162040 | 144061 | 11.1% |
| select-only, prepared (`-S -M prepared`) | 1 | 9792 | 9816 | within noise |
| select-only, prepared (`-S -M prepared`) | 16 | 250386 | 254766 | within noise |
| TPC-B-like | 1 | 1267 | 1258 | 0.7% |
| TPC-B-like | 16 | 19949 | 17664 | 11.5% |

How to read it:

- The 16-client select-only case runs one trivial indexed read per
  transaction at 160k transactions per second; the extension's fixed cost
  per statement and per commit (counting, flushing deltas to shared memory,
  checking thresholds) is then about a tenth of the work. Under callgrind
  the commit callback and the executor hook take about 8% of the
  instructions of a backend; the parse-analysis hook is negligible.
- Statements that do real work dilute that cost: with one client, where
  latency matters more than CPU, it is a few percent at most.
- This machine is noisy: repeated runs of the same case differ by up to
  ±8%, so differences of a few percent are not significant. Rerun the
  script on your hardware for numbers that matter to you.

```sh
bench/run.sh /path/to/pg_config      # env: DURATION, ROUNDS, SCALE, CLIENTS
```

## License

PostgreSQL License, see [LICENSE](LICENSE).
