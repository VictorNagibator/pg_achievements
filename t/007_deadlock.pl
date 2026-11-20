# A real deadlock and a real serialization failure.
use strict;
use warnings;
use FindBin;
use lib $FindBin::RealBin;
use PostgreSQL::Test::Utils;
use Test::More;
use PgaTest;

my $node = pga_node('deadlock', conf => 'deadlock_timeout = 100ms');
$node->safe_psql('postgres', q{
	CREATE ROLE dave LOGIN;
	CREATE TABLE dl (id int PRIMARY KEY, v int);
	INSERT INTO dl VALUES (1, 0), (2, 0);
	GRANT ALL ON dl TO dave;
});

# Sessions must survive the error they are about to get.  query() is used
# instead of query_safe() because achievement notices go to stderr.
my $s1 = $node->background_psql('postgres', extra_params => ['-U', 'dave'],
	on_error_stop => 0);
my $s2 = $node->background_psql('postgres', extra_params => ['-U', 'dave'],
	on_error_stop => 0);
$s1->query('BEGIN; UPDATE dl SET v = 1 WHERE id = 1;');
$s2->query('BEGIN; UPDATE dl SET v = 2 WHERE id = 2;');
$s1->query_until(qr/started/, "\\echo started\nUPDATE dl SET v = 1 WHERE id = 2;\n");
$s2->query('UPDATE dl SET v = 2 WHERE id = 1;');
$s1->query('COMMIT');
$s2->query('ROLLBACK');
$s1->quit;
$s2->quit;

pga_sync($node);
is($node->safe_psql('postgres',
	q{SELECT value FROM achievements.counters WHERE role_name = 'dave' AND counter_key = 'deadlock'}),
	'1', 'deadlock counted once (for the victim)');
is($node->safe_psql('postgres',
	q{SELECT count(*) FROM achievements.awarded WHERE role_name = 'dave' AND code = 'mutual_understanding'}),
	'1', 'mutual_understanding awarded');

# serialization failure
$s1 = $node->background_psql('postgres', extra_params => ['-U', 'dave'],
	on_error_stop => 0);
$s2 = $node->background_psql('postgres', extra_params => ['-U', 'dave'],
	on_error_stop => 0);
$s1->query('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT * FROM dl;');
$s2->query('UPDATE dl SET v = 10 WHERE id = 1;');
$s1->query('UPDATE dl SET v = 11 WHERE id = 1;');
$s1->query('ROLLBACK');
$s1->quit;
$s2->quit;

pga_sync($node);
is($node->safe_psql('postgres',
	q{SELECT count(*) FROM achievements.awarded WHERE role_name = 'dave' AND code = 'serial_conflict'}),
	'1', 'serial_conflict awarded');

$node->stop;
done_testing();
