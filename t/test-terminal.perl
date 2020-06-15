#!/usr/bin/perl
use 5.008;
use strict;
use warnings;
use IO::Pty;
use File::Copy;

# Run @$argv in the background with stdio redirected to $in, $out and $err.
sub start_child {
	my ($argv, $in, $out, $err) = @_;
	my $pid = fork;
	if (not defined $pid) {
		die "fork failed: $!"
	} elsif ($pid == 0) {
		open STDIN, "<&", $in;
		open STDOUT, ">&", $out;
		open STDERR, ">&", $err;
		close $in;
		close $out;
		exec(@$argv) or die "cannot exec '$argv->[0]': $!"
	}
	return $pid;
}

# Wait for $pid to finish.
sub finish_child {
	# Simplified from wait_or_whine() in run-command.c.
	my ($pid) = @_;

	my $waiting = waitpid($pid, 0);
	if ($waiting < 0) {
		die "waitpid failed: $!";
	} elsif ($? & 127) {
		my $code = $? & 127;
		warn "died of signal $code";
		return $code + 128;
	} else {
		return $? >> 8;
	}
}

sub xsendfile {
	my ($out, $in) = @_;

	# Note: the real sendfile() cannot read from a terminal.

	# It is unspecified by POSIX whether reads
	# from a disconnected terminal will return
	# EIO (as in AIX 4.x, IRIX, and Linux) or
	# end-of-file.  Either is fine.
	copy($in, $out, 4096) or $!{EIO} or die "cannot copy from child: $!";
}

sub copy_stdin {
	my ($in) = @_;
	my $pid = fork;
	if (!$pid) {
		xsendfile($in, \*STDIN);
		exit 0;
	}
	close($in);
	return $pid;
}

sub copy_stdio {
	my ($out, $err) = @_;
	my $pid = fork;
	defined $pid or die "fork failed: $!";
	if (!$pid) {
		close($out);
		xsendfile(\*STDERR, $err);
		exit 0;
	}
	close($err);
	xsendfile(\*STDOUT, $out);
	finish_child($pid) == 0
		or exit 1;
}

if ($#ARGV < 1) {
	die "usage: test-terminal program args";
}
$ENV{TERM} = 'vt100';
my $primary_in = new IO::Pty;
my $primary_out = new IO::Pty;
my $primary_err = new IO::Pty;
$primary_in->set_raw();
$primary_out->set_raw();
$primary_err->set_raw();
$primary_in->slave->set_raw();
$primary_out->slave->set_raw();
$primary_err->slave->set_raw();
my $pid = start_child(\@ARGV, $primary_in->slave, $primary_out->slave, $primary_err->slave);
close $primary_in->slave;
close $primary_out->slave;
close $primary_err->slave;
my $in_pid = copy_stdin($primary_in);
copy_stdio($primary_out, $primary_err);
my $ret = finish_child($pid);
# If the child process terminates before our copy_stdin() process is able to
# write all of its data to $primary_in, the copy_stdin() process could stall.
# Send SIGTERM to it to ensure it terminates.
kill 'TERM', $in_pid;
finish_child($in_pid);
exit($ret);
