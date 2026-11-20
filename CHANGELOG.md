# Changelog

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
