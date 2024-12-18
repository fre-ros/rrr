#!/usr/bin/perl -w

package main;

use rrr::rrr_helper;
use rrr::rrr_helper::rrr_message;
use rrr::rrr_helper::rrr_settings;
use rrr::rrr_helper::rrr_debug;

my $dbg = { };
bless $dbg, rrr::rrr_helper::rrr_debug;

my $count = 0;

sub send_message {
	my $message = shift;
	my $server = shift;
	my $port = shift;
	my $count = shift;

	$message->clear_array();

	# Should be filtered out by http_meta_tags_ignore=yes
	$message->push_tag_str ("http_server", $server);
	$message->push_tag_str ("http_port", $port);

	# Should be filtered out by http_request_tags_ignore=yes
	$message->push_tag_str ("http_authority", "authority");
	$message->push_tag_str ("http_request_partials", "partials");

	$message->push_tag_str("my_value", "my_value");

	for (my $i = 0; $i < $count; $i++) {
		$message->send();
	}
}

sub source {
	my $message = shift;

	if ($count < 50) {
		# Use invalid server to create HOL blocking situation
		# in httpclient.
		send_message($message, "1.1.1.1", "9999", 5);
	}
	elsif ($count == 50) {
		# Send message with valid destination. The invalid
		# messages should be graylisted prior to this message
		# getting timed out, which means that these messages
		# should be sent immediately by being bumped in the
		# queue.
		send_message($message, "localhost", "8884", 1);
	}
	else {
		# Done
	}

	$count++;

	return 1;
}
