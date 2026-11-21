# pg_dump / restore keeps progress and custom achievements.
use strict;
use warnings;
use FindBin;
use lib $FindBin::RealBin;
use PostgreSQL::Test::Utils;
use Test::More;
use PgaTest;

my $src = pga_node('dump_src');
$src->safe_psql('postgres', q{
	CREATE ROLE bob LOGIN;
	GRANT CREATE ON SCHEMA public TO bob;
	SELECT achievements.define('vacuumer', 'Vacuumer', 'Run VACUUM FULL.', 'gold', 'vacuum_full', 1);
});
pga_sync($src);
pga_psql($src, 'CREATE TABLE d (id int); VACUUM FULL d; SELECT 1;', user => 'bob');
pga_sync($src);

my $q = q{SELECT string_agg(role_name || ':' || code, ',' ORDER BY role_name, code)
	FROM achievements.awarded WHERE role_name = 'bob'};
my $awards = $src->safe_psql('postgres', $q);
like($awards, qr/bob:vacuumer/, 'custom achievement earned');

my $dump = $src->basedir . '/dump.sql';
$src->command_ok([ 'pg_dump', '-h', $src->host, '-p', $src->port, '-f', $dump, 'postgres' ],
	'pg_dump');

my $dst = pga_node('dump_dst', no_extension => 1);
$dst->safe_psql('postgres', 'CREATE ROLE bob LOGIN');
$dst->command_ok([ 'psql', '-X', '-v', 'ON_ERROR_STOP=1', '-q', '-h', $dst->host,
	'-p', $dst->port, '-f', $dump, 'postgres' ], 'restore');
$dst->restart;
pga_sync($dst);

is($dst->safe_psql('postgres', $q), $awards, 'awards restored');
is($dst->safe_psql('postgres', 'SELECT count(*) FROM achievements.catalog'), '69',
	'builtin catalog not duplicated, custom achievement restored');
is($dst->safe_psql('postgres',
	q{SELECT value FROM achievements.counters WHERE role_name = 'bob' AND counter_key = 'vacuum_full'}),
	'1', 'counters restored');

# restored progress is live: no second award for bob
my (undef, $err) = pga_psql($dst, 'CREATE TABLE d2 (id int); VACUUM FULL d2;', user => 'bob');
unlike($err, qr/Vacuumer|Master of the Table|Welcome/, 'no duplicate awards after restore');

$src->stop;
$dst->stop;
done_testing();
