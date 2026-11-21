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

# Restore into a running server.  The worker writes the counters of the
# restoring session as soon as the extension exists, so COPY meets rows for
# the same keys (e.g. create_extension of the restoring role); a sync()
# right after CREATE EXTENSION makes that happen every time.  The triggers
# merge such rows instead of failing.
my $sql = slurp_file($dump);
like($sql, qr/^CREATE EXTENSION IF NOT EXISTS pg_achievements .*;$/m,
	'dump creates the extension');
$sql =~ s/^(CREATE EXTENSION IF NOT EXISTS pg_achievements .*;)$/$1\nSELECT achievements.sync();/m;
my $racy_dump = $src->basedir . '/dump_racy.sql';
open(my $fh, '>', $racy_dump) or die "could not write $racy_dump: $!";
print $fh $sql;
close($fh);

my $src_creates = $src->safe_psql('postgres',
	q{SELECT value FROM achievements.counters
	  WHERE role_name = current_user AND counter_key = 'create_extension'});

my $dst = pga_node('dump_dst', no_extension => 1);
$dst->safe_psql('postgres', 'CREATE ROLE bob LOGIN');
$dst->command_ok([ 'psql', '-X', '-v', 'ON_ERROR_STOP=1', '-q', '-h', $dst->host,
	'-p', $dst->port, '-f', $racy_dump, 'postgres' ], 'restore');
$dst->restart;
pga_sync($dst);

is($dst->safe_psql('postgres', $q), $awards, 'awards restored');
is($dst->safe_psql('postgres', 'SELECT count(*) FROM achievements.catalog'), '69',
	'builtin catalog not duplicated, custom achievement restored');
is($dst->safe_psql('postgres',
	q{SELECT value FROM achievements.counters WHERE role_name = 'bob' AND counter_key = 'vacuum_full'}),
	'1', 'counters restored');
# the restored value and the CREATE EXTENSION of the restore add up
is($dst->safe_psql('postgres',
	q{SELECT value FROM achievements.counters
	  WHERE role_name = current_user AND counter_key = 'create_extension'}),
	$src_creates + 1, 'restored counter merged with the one written meanwhile');

# restored progress is live: no second award for bob
my (undef, $err) = pga_psql($dst, 'CREATE TABLE d2 (id int); VACUUM FULL d2;', user => 'bob');
unlike($err, qr/Vacuumer|Master of the Table|Welcome/, 'no duplicate awards after restore');

$src->stop;
$dst->stop;
done_testing();
