#!/bin/bash
#
# Throughput cost of pg_achievements, measured with pgbench.
#
# Starts a throw-away cluster and compares two servers on the same data:
#
#   base  the library is not loaded at all (shared_preload_libraries empty);
#   on    the library is preloaded and tracking is on (notices off, they
#         only cost the client).
#
# The baseline is a server without the library, not enabled = off: the hooks
# and the query ids the extension turns on cost something even when nothing
# is counted, and that belongs in the measurement.
#
# Cases: pgbench -S (one indexed SELECT per transaction, CPU bound, the worst
# case for a per-statement hook) with the simple and the prepared protocol,
# and the default TPC-B-like script (5 statements, writes).  All clients
# connect as the same role, so they share its counters in shared memory:
# the most contended setup.
#
# Every case runs ROUNDS times, alternating the order of base and on from
# round to round, after a short warm-up following each restart; the median
# is reported.  Rerun it on your hardware: a laptop or a VM is noisy.
#
# Usage: bench/run.sh [pg_config]
# Environment: DURATION (s, default 15), ROUNDS (5), SCALE (10),
#              CLIENTS ("1 16"), WORKDIR (a new temp dir), PORT (55999)

set -euo pipefail

PG_CONFIG=${1:-pg_config}
BINDIR=$("$PG_CONFIG" --bindir)
DURATION=${DURATION:-15}
ROUNDS=${ROUNDS:-5}
SCALE=${SCALE:-10}
CLIENTS=${CLIENTS:-1 16}
WORKDIR=${WORKDIR:-$(mktemp -d)}
PORT=${PORT:-55999}
DATA=$WORKDIR/data

export PGHOST=$WORKDIR PGPORT=$PORT PGDATABASE=postgres

cleanup() { "$BINDIR/pg_ctl" -D "$DATA" -m immediate stop >/dev/null 2>&1 || true; }
trap cleanup EXIT

"$BINDIR/initdb" -D "$DATA" -A trust --no-sync >/dev/null
cat >> "$DATA/postgresql.conf" <<CONF
listen_addresses = ''
unix_socket_directories = '$WORKDIR'
port = $PORT
max_connections = 50
shared_buffers = 512MB
synchronous_commit = off
pg_achievements.notify = off
include_if_exists = 'mode.conf'
CONF

# restart the server as "base" or "on"
start_mode() {
	if [ "$1" = on ]; then
		echo "shared_preload_libraries = 'pg_achievements'" > "$DATA/mode.conf"
	else
		echo "shared_preload_libraries = ''" > "$DATA/mode.conf"
	fi
	"$BINDIR/pg_ctl" -D "$DATA" -m fast stop >/dev/null 2>&1 || true
	"$BINDIR/pg_ctl" -D "$DATA" -l "$WORKDIR/server.log" -w start >/dev/null
}

start_mode on
"$BINDIR/psql" -qXc 'CREATE EXTENSION pg_achievements'
"$BINDIR/pgbench" -qi -s "$SCALE" >/dev/null 2>&1

# prints the tps of one pgbench run: run_once "<pgbench options>" clients
run_once() {
	# shellcheck disable=SC2086
	"$BINDIR/pgbench" -n $1 -c "$2" -j "$2" -T "$DURATION" 2>/dev/null |
		sed -n 's/^tps = \([0-9.]*\) .*/\1/p'
}

# restart in a mode, warm the cache up, then measure
measure() {
	start_mode "$1"
	# shellcheck disable=SC2086
	"$BINDIR/pgbench" -n $2 -c "$3" -j "$3" -T 3 >/dev/null 2>&1
	run_once "$2" "$3"
}

median() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }

printf '| %-22s | %7s | %10s | %10s | %8s |\n' case clients "base tps" "on tps" cost
printf '| %-22s | %7s | %10s | %10s | %8s |\n' --- ---: ---: ---: ---:
for case in "select-only|-S" "select-only, prepared|-S -M prepared" "TPC-B-like|"; do
	name=${case%%|*}
	opts=${case#*|}
	for clients in $CLIENTS; do
		base=(); on=()
		for round in $(seq "$ROUNDS"); do
			if [ $((round % 2)) -eq 1 ]; then
				base+=("$(measure base "$opts" "$clients")")
				on+=("$(measure on "$opts" "$clients")")
			else
				on+=("$(measure on "$opts" "$clients")")
				base+=("$(measure base "$opts" "$clients")")
			fi
		done
		base_m=$(printf '%s\n' "${base[@]}" | median)
		on_m=$(printf '%s\n' "${on[@]}" | median)
		printf '| %-22s | %7s | %10.0f | %10.0f | %7.1f%% |\n' "$name" "$clients" \
			"$base_m" "$on_m" "$(echo "($base_m - $on_m) * 100 / $base_m" | bc -l)"
	done
done
