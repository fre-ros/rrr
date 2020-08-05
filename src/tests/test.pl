#!/usr/bin/perl -w

package main;

use rrr::rrr_helper;
use rrr::rrr_helper::rrr_message;
use rrr::rrr_helper::rrr_settings;
use rrr::rrr_helper::rrr_debug;

my $dbg = { };
bless $dbg, rrr::rrr_helper::rrr_debug;

$dbg->msg(0, "This is my message\n");
$dbg->dbg(1, "This is my message dbg 1\n");
$dbg->err("This is my error message err\n");

sub config {
	my $settings = shift;

	print "perl5 senders: " . $settings->get("senders") . "\n";

	return 1;
}

sub source {
	my $message = shift;

	$message->{'timestamp'} = $message->{'timestamp'};

	$message->send();

	sleep(1);

	return 1;
}

sub process {
	my $message = shift;

	print "perl5 timestamp: " . $message->{'timestamp'} . "\n";
	print "perl5 old topic: " . $message->{'topic'} . "\n";
	$message->{'topic'} .= "/perl5";
	print "perl5 new topic: " . $message->{'topic'} . "\n";

	my @values = (3, 2, 1);
	my $result = 0;

	# Just call all message XSUB functions to make sure they do not crash
	# At the end, clear "tag" tag from array

	$result += $message->push_tag_blob ("tag", "blob", 4);
	$result += $message->push_tag_str ("tag", "str");
	$result += $message->push_tag_h ("tag", 666);
	$result += $message->push_tag_fixp ("tag", 666);
	$result += $message->push_tag ("tag", \@values);

	my @values_result = $message->get_tag_all ("tag");# Returns array of length 7
	$result += $#values_result + 1;

	$result += $message->set_tag_blob ("tag", "blob", 4);
	$result += $message->set_tag_str ("tag", "str");
	$result += $message->set_tag_fixp ("tag", 666);
	$result += $message->set_tag_h ("tag", 1);

	$result += $message->get_tag_all ("tag");	# Returns array of length 1
	$result += ($message->get_tag_all ("tag"))[0];	# Returns the value 1

	$result += $message->clear_tag ("tag");

	$result += $message->push_tag_fixp("my_fixp", "16#a");
	$result += $message->push_tag_fixp("my_fixp", "10#10");
	$result += $message->push_tag_fixp("my_fixp", ($message->get_tag_all("my_fixp"))[0]);

	if ($result != 22) {
		print ("Result $result<>22\n");
		return 0;
	}

	my @fixps = $message->get_tag_all("my_fixp");
	printf "Fixed points: @fixps\n";

	foreach my $fixp (@fixps) {
		if ($fixp ne "16#000000000a.000000") {
			print "Fixed point failure\n";
			return 0;
		}
	}

	print "Tag names: " . join(",", $message->get_tag_names ()) . "\n";
	print "Tag counts: " . join(",", $message->get_tag_counts ()) . "\n";

	$message->ip_set("127.0.0.1", 666);

	my ($ip_orig, $port_orig) = $message->ip_get();
	$message->ip_set($ip_orig, $port_orig);

	my ($ip, $port) = $message->ip_get();
	print "IP4: $ip:$port\n";

	if ($ip ne "127.0.0.1" || $port != 666) {
		print "IP4 failure";
		return 0;
	}

	$message->ip_set("1::1", 666);

	my ($ip6_orig, $port6_orig) = $message->ip_get();
	$message->ip_set($ip6_orig, $port6_orig);

	my ($ip_6, $port_6) = $message->ip_get();
	print "IP6: $ip_6:$port_6\n";

	if ($ip_6 ne "1::1" || $port_6 != 666) {
		print "IP6 failure\n";
		return 0;
	}

	$message->ip_clear();

	my ($ip_none, $port_none) = $message->ip_get();

	if (defined $ip_none || defined $port_none) {
		print "IP clear failure\n";
	}


	$message->send();

	$message->clear_array();

	return 1;
}
