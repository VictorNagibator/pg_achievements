# A role renamed while one of its sessions runs: progress is kept by role
# name, so the session must go on counting under the new name, which is the
# one the views (current_user) and the tables use.
use strict;
use warnings;
use FindBin;
use lib $FindBin::RealBin;
use PostgreSQL::Test::Utils;
use Test::More;
use PgaTest;

my $node = pga_node('rename');
$node->safe_psql('postgres', 'CREATE ROLE bob LOGIN');
# query_safe() refuses anything on stderr, notices included
$node->safe_psql('postgres', 'ALTER ROLE bob SET pg_achievements.notify = off');

my $bob = $node->background_psql('postgres', extra_params => ['-U', 'bob']);
$bob->query_safe('SELECT 1');

# a role cannot rename itself: another session does it
$node->safe_psql('postgres', 'ALTER ROLE bob RENAME TO bobby');

$bob->query_safe('SELECT 2');
$bob->query_safe('SELECT 3');
$bob->query_safe('SELECT achievements.sync(10000)');
# SELECT 2, SELECT 3 and the SELECT of sync() so far; bob keeps SELECT 1
is($bob->query_safe(
	q{SELECT current_user || ' ' || achievements.counter('stmt_select')}),
	'bobby 3', 'the counter of the session follows the new name');
$bob->quit;
pga_sync($node);

is($node->safe_psql('postgres',
	q{SELECT string_agg(role_name || '=' || value, ' ' ORDER BY role_name)
	  FROM achievements.counters WHERE counter_key = 'stmt_select'
	  AND role_name IN ('bob', 'bobby')}),
	'bob=1 bobby=4', 'statements after the rename are stored under the new name');

$node->stop;
done_testing();
