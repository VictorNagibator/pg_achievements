# The worker restarts after a failure; kill -9 does not damage the tables.
use strict;
use warnings;
use FindBin;
use lib $FindBin::RealBin;
use PostgreSQL::Test::Utils;
use Test::More;
use PgaTest;

# the test framework disables restart_after_crash, but here the crash
# recovery cycle triggered by kill -9 of the worker is the point
my $node = pga_node('worker', conf => 'restart_after_crash = on');
my $worker_pid = q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'pg_achievements'};

my $pid = $node->safe_psql('postgres', $worker_pid);
ok($pid > 0, 'worker is running');

# terminated worker: the server goes on, the launcher restarts the worker
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
$node->safe_psql('postgres', 'CREATE TABLE w1 (id int)');
is($node->safe_psql('postgres', 'SELECT 1'), '1', 'server works without the worker');
ok($node->poll_query_until('postgres',
	"SELECT count(*) = 1 AND min(pid) <> $pid FROM pg_stat_activity WHERE backend_type = 'pg_achievements'"),
	'worker restarted');
pga_sync($node);
is($node->safe_psql('postgres',
	q{SELECT value FROM achievements.counters
	  WHERE role_name = current_user AND counter_key = 'create_table'}),
	'1', 'events counted while the worker was down are written');

# kill -9 of the worker makes the postmaster reinitialize shared memory
$pid = $node->safe_psql('postgres', $worker_pid);
kill 'KILL', $pid;
ok($node->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'pg_achievements'"),
	'worker back after kill -9');
pga_sync($node);
is($node->safe_psql('postgres',
	q{SELECT value FROM achievements.counters
	  WHERE role_name = current_user AND counter_key = 'create_table'}),
	'1', 'counters intact after kill -9');
is($node->safe_psql('postgres',
	'SELECT count(*) FROM achievements.awarded a JOIN achievements.catalog c USING (code)'),
	$node->safe_psql('postgres', 'SELECT count(*) FROM achievements.awarded'),
	'awarded rows consistent');

# kill -9 of the postmaster
$node->stop('immediate');
$node->start;
pga_sync($node);
$node->safe_psql('postgres', 'CREATE TABLE w2 (id int)');
pga_sync($node);
is($node->safe_psql('postgres',
	q{SELECT value FROM achievements.counters
	  WHERE role_name = current_user AND counter_key = 'create_table'}),
	'2', 'tracking continues after crash recovery');

# pg_terminate_backend() of the launcher: it comes back, does not start a
# second flush worker next to the running one, and restarts that one later
my $launcher_pid = q{SELECT pid FROM pg_stat_activity
	WHERE backend_type = 'pg_achievements launcher'};
my $lpid = $node->safe_psql('postgres', $launcher_pid);
$pid = $node->safe_psql('postgres', $worker_pid);
$node->safe_psql('postgres', "SELECT pg_terminate_backend($lpid)");
ok($node->poll_query_until('postgres',
	"SELECT count(*) = 1 AND min(pid) <> $lpid FROM pg_stat_activity
	 WHERE backend_type = 'pg_achievements launcher'"),
	'launcher restarted');
sleep(2);
is($node->safe_psql('postgres',
	"SELECT string_agg(pid::text, ',') FROM pg_stat_activity
	 WHERE backend_type = 'pg_achievements'"),
	$pid, 'the running flush worker stays the only one');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
ok($node->poll_query_until('postgres',
	"SELECT count(*) = 1 AND min(pid) <> $pid FROM pg_stat_activity
	 WHERE backend_type = 'pg_achievements'"),
	'the new launcher restarts the flush worker');
pga_sync($node);

# the database of the extension may be created after the server started
my $late = pga_node('late', no_extension => 1,
	conf => "pg_achievements.database = 'late_db'");
ok($late->log_contains('waiting for database "late_db"'), 'launcher waits');
$late->safe_psql('postgres', 'CREATE DATABASE late_db');
$late->safe_psql('late_db', 'CREATE EXTENSION pg_achievements');
is($late->safe_psql('late_db', 'SELECT achievements.sync(10000)'), 't',
	'worker started for a database created later');

$node->stop;
$late->stop;
done_testing();
