#!/usr/bin/perl

# Tests for the shared shell parser (lib-shell-parser.pl).

use strict;
use warnings;
use File::Basename;

my $_lib = dirname($0) . "/lib-shell-parser.pl";
$_lib = "./$_lib" unless $_lib =~ m{^/};
do $_lib or die "$0: failed to load $_lib: $@$!\n";

my $rc = 0;

sub check {
	my ($desc, $body, $want_token, $want_line) = @_;
	my $parser = ShellParser->new(\$body);
	my @tokens = $parser->parse();
	for my $t (reverse @tokens) {
		next unless $t->[0] eq $want_token && defined $t->[3];
		if ($t->[3] != $want_line) {
			print STDERR "FAIL: $desc: " .
				"'$want_token' at line $t->[3], " .
				"expected line $want_line\n";
			$rc = 1;
		}
		return;
	}
	print STDERR "FAIL: $desc: token '$want_token' not found\n";
	$rc = 1;
}

# Multi-line $() inside a dq-string: MARKER should be at line 3.
check('dq-string with multi-line $()', <<'BODY', 'MARKER', 3);
	x="$(echo one
	echo two)" &&
	MARKER here
BODY

# Two multi-line $() substitutions: verifies drift does not accumulate.
# MARKER should be at line 5.
check('two dq-string $()', <<'BODY', 'MARKER', 5);
	x="$(echo a
	b)" &&
	y="$(echo c
	d)" &&
	MARKER here
BODY

# $() outside a dq-string: no double-counting either way.
# MARKER should be at line 3.
check('bare $() spanning lines', <<'BODY', 'MARKER', 3);
	x=$(echo one
	echo two) &&
	MARKER here
BODY

exit $rc;
