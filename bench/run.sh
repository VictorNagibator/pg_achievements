#!/bin/bash
#
# Throughput overhead of pg_achievements, measured with pgbench.
#
# Starts a throw-away cluster with the extension preloaded and runs
# pgbench -S (read-only) and the default TPC-B-like script with 1 and 16
# clients, alternating pg_achievements.enabled = off / on.  Every case is
# repeated ROUNDS times and the median is reported, which keeps the noise of
# a laptop or a VM under control.
#
# Usage: bench/run.sh [pg_config]   (env: DURATION, ROUNDS, SCALE, WORKDIR)

set -euo pipefail

PG_CONFIG=${1:-pg_config}
BINDIR=$("$PG_CONFIG" --bindir)
DURATION=${DURATION:-20}
ROUNDS=${ROUNDS:-3}
SCALE=${SCALE:-10}
WORKDIR=${WORKDIR:-$(mktemp -d)}
PORT=${PORT:-55999}
DATA=$WORKDIR/data

export PGHOST=$WORKDIR PGPORT=$PORT PGDATABASE=postgres

cleanup() { "$BINDIR/pg_ctl" -D "$DATA" -m immediate stop >/dev/null 2>&1 || true; }
trap cleanup EXIT

"$BINDIR/initdb" -D "$DATA" -A trust --no-sync >/dev/null
cat >> "$DATA/postgresql.conf" <<CONF
shared_preload_libraries = 'pg_achievements'
listen_addresses = ''
unix_socket_directories = '$WORKDIR'
port = $PORT
max_connections = 50
shared_buffers = 512MB
synchronous_commit = off
pg_achievements.notify = off
CONF
"$BINDIR/pg_ctl" -D "$DATA" -l "$WORKDIR/server.log" -w start >/dev/null
"$BINDIR/psql" -qXc 'CREATE EXTENSION pg_achievements'
"$BINDIR/pgbench" -qi -s "$SCALE" >/dev/null 2>&1

set_enabled() {
	"$BINDIR/psql" -qXc "ALTER SYSTEM SET pg_achievements.enabled = $1" -c 'SELECT pg_reload_conf()' >/dev/null
	sleep 1
}

# prints the tps of one pgbench run
run_once() {
	"$BINDIR/pgbench" -n $1 -c "$2" -j "$2" -T "$DURATION" 2>/dev/null |
		sed -n 's/^tps = \([0-9.]*\) .*/\1/p'
}

median() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }

printf '%-10s %7s %12s %12s %8s\n' script clients off_tps on_tps overhead
for script in "-S" "" ; do
	name=${script:-tpcb}
	[ "$name" = "-S" ] && name=select-only
	for clients in 1 16; do
		off=(); on=()
		for _ in $(seq "$ROUNDS"); do
			set_enabled off; off+=("$(run_once "$script" "$clients")")
			set_enabled on;  on+=("$(run_once "$script" "$clients")")
		done
		off_m=$(printf '%s\n' "${off[@]}" | median)
		on_m=$(printf '%s\n' "${on[@]}" | median)
		printf '%-10s %7s %12.0f %12.0f %7.2f%%\n' "$name" "$clients" "$off_m" "$on_m" \
			"$(echo "($off_m - $on_m) * 100 / $off_m" | bc -l)"
	done
done
