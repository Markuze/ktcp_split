#!/usr/bin/perl -w

use strict;
use autodie;
use IO::Socket;
use Time::HiRes;
use Regexp::Common qw/ net number /;
$| = 1; #?

#my $serverip = '127.0.0.1';
my $serverip = '10.154.0.18';
my $serverport = 8080;
my $size = 65536 ;


$serverport = $ARGV[0] if defined $ARGV[0];

printf " stream to $serverport\n";

sub parse_size {
	my $base = shift;
	my $str = shift;
	my $mult = 1;

	if (defined $str) {
		$mult = 1000 if ($str eq 'k');
		$mult = 1024 if ($str eq 'K');
		$mult = (1000 * 1000) if ($str eq 'm');
		$mult = (1024 * 1024) if ($str eq 'M');
	}
	return $base * $mult;
}

sub parse_line {
	my $line = shift;
	if ($line =~ /server\s+($RE{net}{IPv4})/) {
		$serverip = $1 if defined ($1);
	}

	if ($line =~ /port\s+(\d{1,5})/) {
		$serverport = $1 if defined ($1);
	}

	if ($line =~ /size\s+(\d+)([kKmM]*)/) {
		my $base = $1 if defined ($1);
		my $str = $2 if defined ($2);
		$size = parse_size($base, $str) if defined $base;
	}
}

sub parse_config {
	open (my $fh, "<", './config');
	while (<$fh>) {
		parse_line($_);
	}
}

sub get_params {
	if ( -r "./config" ) {
		return parse_config();
	}
}

get_params();
printf "$serverip $serverport $size\n";

my $start_time = Time::HiRes::gettimeofday();
my $message = IO::Socket::INET->new(Proto=>"tcp", PeerPort=>$serverport,
				     PeerAddr=>$serverip);
my $time = Time::HiRes::gettimeofday();
printf "connected...%f\n", $time - $start_time;
my $bytes = 0;
my $last_time = $start_time;

while (($time - $start_time) < 160) {

	if (($time - $last_time) > 1) {
		$last_time = $time;
		printf "%f) %.2f\n", $last_time - $start_time, $bytes/(128 * 1024);
		$bytes = 0;
	}
	$message->send("x"x$size);
	$bytes += $size;
	$time = Time::HiRes::gettimeofday();
}

printf "Sent [%.2fGB], closing...\n", $bytes/(1024*1024);
$message->close();
