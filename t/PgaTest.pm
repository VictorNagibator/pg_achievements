# Common helpers for the pg_achievements TAP tests.
package PgaTest;

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Exporter 'import';

our @EXPORT = qw(pga_node pga_psql pga_sync);

# A node with the extension preloaded and created in "postgres".
sub pga_node
{
	my ($name, %opts) = @_;
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init(allows_streaming => $opts{allows_streaming} // 0);
	my $preload = $opts{preload} // 'pg_achievements';
	$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$preload'
pg_achievements.flush_interval_ms = 100
pg_achievements.notice_style = 'ascii'
max_connections = 120
});
	$node->append_conf('postgresql.conf', $opts{conf}) if $opts{conf};
	$node->start;
	unless ($opts{no_extension})
	{
		$node->safe_psql('postgres', 'CREATE EXTENSION pg_achievements');
		pga_sync($node);
	}
	return $node;
}

# Run SQL, return (stdout, stderr).
sub pga_psql
{
	my ($node, $sql, %opts) = @_;
	my ($out, $err);
	$node->psql($opts{db} // 'postgres', $sql,
		stdout => \$out, stderr => \$err,
		extra_params => $opts{user} ? ['-U', $opts{user}] : []);
	return ($out, $err);
}

sub pga_sync
{
	my ($node) = @_;
	is($node->safe_psql('postgres', 'SELECT achievements.sync(10000)'),
		't', 'worker flushed');
}

1;
