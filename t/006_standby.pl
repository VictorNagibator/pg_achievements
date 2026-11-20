# On a hot standby the worker never writes to the tables.
use strict;
use warnings;
use FindBin;
use lib $FindBin::RealBin;
use PostgreSQL::Test::Utils;
use Test::More;
use PgaTest;

my $primary = pga_node('primary', allows_streaming => 1);
$primary->safe_psql('postgres', 'CREATE ROLE carol LOGIN; SELECT 1');
pga_sync($primary);
$primary->backup('bk');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bk', has_streaming => 1);
$standby->start;
$primary->wait_for_catchup($standby);

is($standby->safe_psql('postgres', 'SELECT achievements.sync(10000)'), 't',
	'worker runs on the standby');

my ($out, $err) = pga_psql($standby,
	"SELECT 1; SELECT achievements.counter('stmt_select');", user => 'carol');
like($err, qr/Hello, World/, 'achievements are reported on the standby');
is($standby->safe_psql('postgres', 'SELECT achievements.sync(10000)'), 't', 'sync on standby');
ok(!$standby->log_contains('read-only transaction'), 'no write attempts on the standby');
is($standby->safe_psql('postgres',
	q{SELECT count(*) FROM achievements.awarded WHERE role_name = 'carol'}),
	'0', 'nothing written');

$standby->stop;
$primary->stop;
done_testing();
