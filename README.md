# pg_achievements

[![CI](https://github.com/VictorNagibator/pg_achievements/actions/workflows/ci.yml/badge.svg)](https://github.com/VictorNagibator/pg_achievements/actions/workflows/ci.yml)
[![PostgreSQL 16 | 17 | 18](https://img.shields.io/badge/PostgreSQL-16%20%7C%2017%20%7C%2018-336791?logo=postgresql&logoColor=white)](#requirements)
[![PGXN version](https://badge.fury.io/pg/pg_achievements.svg)](https://pgxn.org/dist/pg_achievements/)
[![License: PostgreSQL](https://img.shields.io/badge/license-PostgreSQL-blue)](LICENSE)

Game achievements for PostgreSQL users. The extension watches what roles do in
the database and hands out achievements in four tiers (bronze, silver, gold,
platinum). The notice arrives right in the client, in the same session:

```text
postgres=> UPDATE accounts SET balance = 0;
NOTICE:  🥇 [GOLD] Achievement unlocked: Forgot WHERE (+50)
DETAIL:  UPDATE without a WHERE clause. Total score: 135.
UPDATE 1042
```

Some achievements nudge you toward PostgreSQL features (`EXPLAIN ANALYZE`,
recursive CTEs, window functions, partitioning, `CREATE INDEX CONCURRENTLY`).
Others gently mock classic mistakes, like the `UPDATE` above or a Friday
evening deploy. All 46 builtin achievements are listed in
[docs/ACHIEVEMENTS.md](docs/ACHIEVEMENTS.md).

## Requirements

- PostgreSQL 16, 17 or 18 on Linux; the main development version is 18;
- the server must preload the library (`shared_preload_libraries`), so managed
  services such as RDS or Cloud SQL are not supported;
- server headers and PGXS: `postgresql-server-dev-NN` on Debian/Ubuntu,
  plus `libkrb5-dev` (the server headers include the GSSAPI header when
  PostgreSQL is built with GSSAPI, as the PGDG packages are).

There are no other dependencies.

## Installation

From [PGXN](https://pgxn.org/dist/pg_achievements/):

```sh
pgxn install pg_achievements
```

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

## Usage

```sql
-- my achievements
SELECT tier, title, points, awarded_at FROM achievements.my;

-- everything with my progress; secret descriptions stay hidden until unlocked
SELECT code, title, current, threshold, unlocked FROM achievements.progress;

-- leaderboard of all roles that did not opt out
SELECT * FROM achievements.leaderboard;

-- the catalog without hidden fields
SELECT * FROM achievements.catalog_public;

-- one counter of the current role, including events not flushed yet
SELECT achievements.counter('stmt_select');

-- wait until the worker has written everything to the tables
SELECT achievements.sync();
```

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
| `achievements.define(code, title, description, tier, counter_key, threshold)` | superuser | Adds a custom achievement on an existing counter; points follow the tier (10/25/50/100) |
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

Counting rules:

- Only top-level statements count unless `track_nested` is on.
- `update_no_where`, `delete_no_where`, `null_eq` and `select_star` count at
  parse analysis, because the attempt is what matters. Everything else counts
  only when the statement finishes successfully.
- Statements that change nothing (`DROP TABLE IF EXISTS` on a missing table,
  `CREATE TABLE IF NOT EXISTS` or `CREATE EXTENSION IF NOT EXISTS` on an
  existing object) do not count.
- The role is `current_user` (`GetUserId()`), so after `SET ROLE` events go to
  the new role.
- Times of day use the session `timezone`.
- Background processes (autovacuum, replication, the worker itself) are never
  tracked.

## Privacy and security

- Query texts, parameters and object names are never stored or logged. The
  extension keeps only counters and award facts, and its notices and warnings
  are sent with `errhidestmt`.
- The tables are not readable by `PUBLIC`. Ordinary roles use the views,
  which only show their own progress (the leaderboard shows scores of all
  roles that did not opt out).
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
- A prepared statement is analyzed once, so its parse-time features
  (`SELECT *`, CTEs, joins, …) count at its first execution only. The same
  goes for statements cached by PL/pgSQL with `track_nested`.
- Client-side meta-commands such as `\d` are invisible; the server only sees
  the SQL they send.
- `pg_dump` saves progress and custom achievements. After restoring into a
  running server, restart it so the worker reloads the tables.

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
needed. The TAP tests cover restarts and crashes, `kill -9` of the worker,
50 sessions racing for one award, `pg_dump`/restore, `pg_stat_statements` in
both preload orders, hot standby and real deadlocks.

## Performance

`bench/run.sh [pg_config]` measures the throughput loss with pgbench,
comparing `pg_achievements.enabled = off` and `on` (median of several runs).
PostgreSQL 18.7 built with `-O2`, scale 10, 5 × 15 s per case, 12 vCPU under
WSL2:

| Script | Clients | Overhead |
| --- | --- | --- |
| select-only (`-S`) | 1 | 1.4% |
| select-only (`-S`) | 16 | 1.3% |
| TPC-B-like | 1 | within noise (−5.1%) |
| TPC-B-like | 16 | 2.4%, within noise |

The TPC-B-like runs are I/O bound and vary by about ±5% between runs on this
machine (the extension "won" the single-client case), so they cannot confirm
the 2% target either way; the CPU-bound select-only case does. Rerun the
script on your hardware for numbers that matter to you.

## License

PostgreSQL License, see [LICENSE](LICENSE).
