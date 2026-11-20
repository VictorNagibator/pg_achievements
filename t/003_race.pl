# 50 concurrent sessions of one role hit the same threshold: exactly one award.
use strict;
use warnings;
use FindBin;
use lib $FindBin::RealBin;
use PostgreSQL::Test::Utils;
use Test::More;
use PgaTest;

my $node = pga_node('race');
$node->safe_psql('postgres', q{
	CREATE ROLE racer LOGIN;
	CREATE TABLE r (id int);
	GRANT ALL ON r TO racer;
});

# every client connects (welcome) and runs an UPDATE without WHERE (forgot_where)
my $script = $node->basedir . '/race.sql';
PostgreSQL::Test::Utils::append_to_file($script, "UPDATE r SET id = 1;\n");

my ($stdout, $stderr);
IPC::Run::run(
	[ 'pgbench', '-n', '-c', '50', '-j', '10', '-t', '1', '-f', $script,
	  '-U', 'racer', '-h', $node->host, '-p', $node->port, 'postgres' ],
	'>', \$stdout, '2>', \$stderr);
like($stdout, qr/number of transactions actually processed: 50/, 'pgbench ran');

my @welcome = ($stderr =~ /Achievement unlocked: Welcome/g);
my @where = ($stderr =~ /Achievement unlocked: Forgot WHERE \(/g);
is(scalar @welcome, 1, 'one welcome notice for 50 connections');
is(scalar @where, 1, 'one forgot_where notice for 50 sessions');

pga_sync($node);
is($node->safe_psql('postgres',
	q{SELECT code || ':' || count(*) FROM achievements.awarded
	  WHERE role_name = 'racer' GROUP BY code ORDER BY code}),
	"forgot_where:1\nwelcome:1", 'each achievement stored once');
# pgbench opens one extra connection before starting the clients
is($node->safe_psql('postgres',
	q{SELECT counter_key || '=' || value FROM achievements.counters
	  WHERE role_name = 'racer' ORDER BY counter_key}),
	"connect=51\nupdate_no_where=50", 'all events counted');

$node->stop;
done_testing();
