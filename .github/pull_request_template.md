## What and why

<!-- What does this change, and which problem does it solve? Link the issue: "Fixes #123". -->

## How it was tested

<!-- Tests added or changed, PostgreSQL versions tried locally. -->

## Checklist

- [ ] `make PGA_TESTING=1 installcheck` passes (regression and TAP tests); CI is green on PostgreSQL 16–19
- [ ] New behaviour or a fixed bug is covered by a test in `test/sql` or `t/`
- [ ] `CHANGELOG.md` has an entry under "Unreleased"
- [ ] `README.md` is updated if counters, settings, the SQL API or the counting rules changed
- [ ] Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/)

### Only if it applies

- [ ] **SQL changes**: the install script of the new version and an upgrade script `pg_achievements--OLD--NEW.sql` both have them, and `09_upgrade` still finds the upgraded catalog equal to a fresh install
- [ ] **New achievement**: it is in the builtin catalog with a tier and points that match the others, and `docs/ACHIEVEMENTS.md` is regenerated with `docs/gen_achievements.py`
- [ ] **New counter**: added at the end of `PGA_COUNTER_LIST` (existing keys are stored in the tables and are never renamed or reordered)
- [ ] **Hooks**: no query text or object name is stored or logged, and nothing in a hook can throw on input from the user (an error there disables tracking for the session)
