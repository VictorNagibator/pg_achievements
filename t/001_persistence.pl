# Progress survives a clean restart and a crash of the server.
use strict;
use warnings;
use FindBin;
use lib $FindBin::RealBin;
use PostgreSQL::Test::Utils;
use Test::More;
use PgaTest;

my $node = pga_node('persist');
# since PostgreSQL 15 ordinary roles cannot create tables in public by default
$node->safe_psql('postgres', 'CREATE ROLE alice LOGIN; GRANT CREATE ON SCHEMA public TO alice');

my (undef, $err) = pga_psql($node,
	"CREATE TABLE t (id int); UPDATE t SET id = 1; SELECT 1;", user => 'alice');
like($err, qr/Achievement unlocked: Welcome/, 'welcome at first connection');
like($err, qr/\[GOLD\] Achievement unlocked: Forgot WHERE \(\+50\)/, 'forgot_where notice');
pga_sync($node);

my $q_counters = q{SELECT counter_key || '=' || value FROM achievements.counters
	WHERE role_name = 'alice' ORDER BY counter_key};
# TAP tests run with the real clock: ignore achievements that depend on the
# time of day or the day of the week
my $q_awarded = q{SELECT string_agg(code, ',' ORDER BY code) FROM achievements.awarded
	WHERE role_name = 'alice'
	  AND code NOT IN ('friday_deploy', 'night_owl', 'night_regular')};
my $counters = $node->safe_psql('postgres', $q_counters);
my $awarded = $node->safe_psql('postgres', $q_awarded);
is($awarded, 'first_table,forgot_where,hello_world,welcome', 'awards written');

# clean restart
$node->restart;
pga_sync($node);
is($node->safe_psql('postgres', $q_counters), $counters, 'counters survive restart');
is($node->safe_psql('postgres', $q_awarded), $awarded, 'awards survive restart');

# shared memory was reloaded: the same event does not award twice, and the
# counter continues from the stored value
(undef, $err) = pga_psql($node, "UPDATE t SET id = 2;", user => 'alice');
unlike($err, qr/Achievement unlocked/, 'no second award after restart');
is($node->safe_psql('postgres',
	q{SET ROLE alice; SELECT achievements.counter('update_no_where')}),
	'2', 'counter continues after restart');

# crash: everything flushed before the crash is kept
pga_sync($node);
$node->stop('immediate');
$node->start;
pga_sync($node);
is($node->safe_psql('postgres',
	q{SELECT value FROM achievements.counters
	  WHERE role_name = 'alice' AND counter_key = 'update_no_where'}),
	'2', 'flushed counters survive a crash');
is($node->safe_psql('postgres', $q_awarded), $awarded, 'awards survive a crash');

# a rolled back transaction does not take back the achievement
(undef, $err) = pga_psql($node,
	"BEGIN; DELETE FROM t; ROLLBACK;", user => 'alice');
like($err, qr/Forgot WHERE, Part 2/, 'notice inside a rolled back transaction');
pga_sync($node);
is($node->safe_psql('postgres',
	q{SELECT count(*) FROM achievements.awarded
	  WHERE role_name = 'alice' AND code = 'forgot_where_again'}),
	'1', 'award kept after rollback');

$node->stop;
done_testing();
