# Statements prepared with the extended query protocol (pgbench -M prepared,
# and drivers such as pgx, asyncpg or JDBC) are analyzed once and executed
# many times, in other transactions: every execution must count.
use strict;
use warnings;
use FindBin;
use lib $FindBin::RealBin;
use PostgreSQL::Test::Utils;
use Test::More;
use PgaTest;

my $node = pga_node('prepared');
$node->safe_psql('postgres', 'CREATE TABLE p_t (id int PRIMARY KEY)');
$node->safe_psql('postgres', 'INSERT INTO p_t SELECT generate_series(1, 10)');
$node->safe_psql('postgres', 'CREATE ROLE pat LOGIN');
$node->safe_psql('postgres', 'GRANT SELECT ON p_t TO pat');

my $script = $node->basedir . '/prepared.sql';
PostgreSQL::Test::Utils::append_to_file($script,
	"WITH x AS (SELECT id FROM p_t) SELECT count(*) FROM x JOIN p_t USING (id);\n");

# one transaction per execution, the statement is parsed only once
$node->command_ok(
	[ 'pgbench', '-n', '-M', 'prepared', '-t', '25', '-U', 'pat', '-f', $script,
	  'postgres' ],
	'pgbench with prepared statements');
pga_sync($node);

is($node->safe_psql('postgres',
	q{SELECT string_agg(counter_key || '=' || value, ' ' ORDER BY counter_key)
	  FROM achievements.counters
	  WHERE role_name = 'pat'
	    AND counter_key IN ('stmt_select', 'cte_any', 'join_count_max')}),
	'cte_any=25 join_count_max=1 stmt_select=25',
	'every execution of the prepared statement counts');
ok($node->safe_psql('postgres',
	q{SELECT count(*) FROM achievements.counters
	  WHERE role_name = 'pat' AND counter_key = 'streak_days'}) == 1,
	'a session with prepared statements only is an active day');

$node->stop;
done_testing();
