#!/bin/bash
# Run once by the postgres image entrypoint on a fresh data directory.
# The extension lives in pg_achievements.database, "postgres" by default,
# whatever POSTGRES_DB says.
set -euo pipefail
psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname postgres \
     -c 'CREATE EXTENSION IF NOT EXISTS pg_achievements'
