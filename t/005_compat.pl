# Works next to pg_stat_statements in either order of shared_preload_libraries.
use strict;
use warnings;
use FindBin;
use lib $FindBin::RealBin;
use PostgreSQL::Test::Utils;
use Test::More;
use PgaTest;

foreach my $preload ('pg_stat_statements,pg_achievements',
	'pg_achievements,pg_stat_statements')
{
	(my $name = $preload) =~ s/,/_/;
	my $node = pga_node($name, preload => $preload);
	$node->safe_psql('postgres', 'CREATE EXTENSION pg_stat_statements');

	my (undef, $err) = pga_psql($node,
		'WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 3) SELECT count(*) FROM r;');
	like($err, qr/Recursion: See Recursion/, "$preload: achievement");
	is($node->safe_psql('postgres',
		q{SELECT count(*) > 0 FROM pg_stat_statements WHERE query LIKE 'WITH RECURSIVE%'}),
		't', "$preload: pg_stat_statements still records queries");

	# nesting levels of both extensions stay in sync
	$node->safe_psql('postgres', q{
		CREATE FUNCTION f() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RETURN 1; END $$;
		SELECT f() FROM generate_series(1, 3);
	});
	is($node->safe_psql('postgres', 'SELECT 1'), '1', "$preload: server healthy");
	$node->stop;
}

done_testing();
