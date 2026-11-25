# New awards are announced with NOTIFY, and their rarity shows in the
# notice and in the achievements.rarity view.
use strict;
use warnings;
use FindBin;
use lib $FindBin::RealBin;
use PostgreSQL::Test::Utils;
use Test::More;
use PgaTest;

my $node = pga_node('notify');
$node->safe_psql('postgres', q{
	CREATE ROLE alice LOGIN;
	CREATE ROLE bob LOGIN;
	CREATE ROLE carol LOGIN;
	GRANT ALL ON SCHEMA public TO alice, bob, carol;
	ALTER ROLE bob SET pg_achievements.opt_out = on;
});

# psql prints notifications that arrived during a command after it; the
# worker commits the award before sync() returns, so the last SELECT is
# always late enough to see it.
sub listen_and_grant
{
	my ($channel, $role, $code) = @_;
	my ($out, $err) = pga_psql($node, qq{
		LISTEN $channel;
		SELECT achievements.grant('$role', '$code');
		SELECT achievements.sync(10000);
		SELECT 1;
	});
	return $out;
}

my $out = listen_and_grant('achievements', 'alice', 'yolo');
like($out, qr/Asynchronous notification "achievements" with payload "\{.*"role" : "alice", "code" : "yolo", "title" : "You Only Live Once", "tier" : "gold", "points" : 50, "awarded_at" : "[^"]+"\}"/,
	'new award is announced with a JSON payload');
unlike($out, qr/Drop a database/, 'payload has no description');

$out = listen_and_grant('achievements', 'alice', 'yolo');
unlike($out, qr/Asynchronous notification/, 'an award held already is not announced again');

$out = listen_and_grant('achievements', 'bob', 'yolo');
unlike($out, qr/Asynchronous notification/, 'opted-out role is not announced');

# the channel is a SIGHUP setting
$node->append_conf('postgresql.conf', "pg_achievements.notify_channel = 'trophies'");
$node->reload;
$out = listen_and_grant('trophies', 'alice', 'time_machine');
like($out, qr/Asynchronous notification "trophies" .*"code" : "time_machine"/,
	'notify_channel picks the channel');

$node->append_conf('postgresql.conf', "pg_achievements.notify_channel = ''");
$node->reload;
$out = listen_and_grant('trophies', 'alice', 'marathon');
unlike($out, qr/Asynchronous notification/, 'empty notify_channel turns announcements off');

my ($o, $err) = pga_psql($node,
	"ALTER SYSTEM SET pg_achievements.notify_channel = '" . ('x' x 64) . "'");
like($err, qr/must be shorter than 64 bytes/, 'too long channel name is refused');

# rarity in the notice: alice is the first to create a table, carol second
(undef, $err) = pga_psql($node, 'CREATE TABLE alice_t (id int)', user => 'alice');
like($err, qr/Achievement unlocked: First Table.*\nDETAIL: .*\nYou are the first of \d+ players to unlock it\./s,
	'first holder is told so');
(undef, $err) = pga_psql($node, 'CREATE TABLE carol_t (id int)', user => 'carol');
like($err, qr/Achievement unlocked: First Table.*\nUnlocked by 2 of \d+ players \(\d+%\)\./s,
	'next holder sees the share of players');
pga_sync($node);

# the view: bob opted out, so he is neither a holder nor a player
is($node->safe_psql('postgres', q{
	SELECT holders, players = (SELECT count(*) FROM (
	           SELECT role_name FROM achievements.counters
	           UNION SELECT role_name FROM achievements.awarded) r
	       WHERE role_name <> 'bob')
	FROM achievements.rarity WHERE code = 'yolo'}),
	'1|t', 'rarity counts holders and players without opted-out roles');
is($node->safe_psql('postgres', q{
	SELECT holders FROM achievements.rarity WHERE code = 'first_table'}),
	'2', 'rarity sees both table creators');
is($node->safe_psql('postgres', q{
	SELECT count(*) FROM achievements.rarity}),
	$node->safe_psql('postgres', q{
	SELECT count(*) FROM achievements.catalog c
	WHERE enabled AND NOT achievements._counter_unsupported(c.counter_key)}),
	'rarity lists every enabled achievement this server supports');

$node->stop;
done_testing();
