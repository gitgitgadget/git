#!/usr/bin/perl

# Check test scripts for style violations that can be detected
# mechanically, such as using bare 'grep' where test_grep should
# be used.  Use --fix to automatically apply suggested replacements.
#
# Detection uses parsed tokens from the shared shell parser for
# correct handling of heredocs, $(...), pipes, and quoting.
# Fixes modify the original file text to preserve formatting.

use strict;
use warnings;
use File::Basename;
# Force LF output so check-lint-style's diff against the
# pre-committed .expect files works on Windows.
binmode(STDOUT, ':unix');
binmode(STDERR, ':unix');

my $fix_mode = 0;
if (@ARGV && $ARGV[0] eq '--fix') {
	$fix_mode = 1;
	shift @ARGV;
}

# Load the shared shell parser (Lexer, ShellParser, ScriptParser).
my $_lib = dirname($0) . "/lib-shell-parser.pl";
$_lib = "./$_lib" unless $_lib =~ m{^/};
do $_lib or die "$0: failed to load $_lib: $@$!\n";

# LintParser is a subclass of ScriptParser which runs lint rules
# on each test body.  Per-file state (file name, raw lines, dirty
# flag) is stored on the instance before calling parse().
#
# Subroutines defined below (parse_commands, check_test_grep_negation,
# etc.) are in package main and called with the main:: prefix.
# File-scoped lexicals ($fix_mode, $has_fixable, etc.) are visible
# across packages since 'package' does not introduce a new scope.
package LintParser;
our @ISA = ('ScriptParser');

package main;

my $exit_code = 0;
my $has_fixable = 0;

sub err {
	my ($file, $lineno, $line, $msg, %opts) = @_;
	$line =~ s/^\s+//;
	$line =~ s/\s+$//;
	$line =~ s/\s+/ /g;
	my $prefix = ($fix_mode && $opts{fixable}) ? 'fixed' : 'error';
	print "$file:$lineno: $prefix: $msg: $line\n";
	$exit_code = 1 unless $fix_mode && $opts{fixable};
}

# Report a lint violation found by a rule.  In --fix mode, apply
# the regex substitution on the raw line and report success.
# Otherwise just report.  Returns 1 if the line was modified.
sub report_violation {
	my ($file, $cmd, $line_ref, $match, $fix, $from) = @_;
	my $lineno = $cmd->{lineno};
	my $display = join(' ', @{$cmd->{tokens}});
	$has_fixable++;  # count for the "--fix" hint
	if ($fix_mode) {
		if ($$line_ref =~ s/$match/$fix/) {
			err $file, $lineno, $display,
				"replace '$from' with '$fix'",
				fixable => 1;
			return 1;
		}
		err $file, $lineno, $display,
			"replace '$from' with '$fix' (could not auto-fix)";
	} else {
		err $file, $lineno, $display,
			"replace '$from' with '$fix'";
	}
	return 0;
}

# Split a token stream into commands at &&, ||, ;;, and \n.
sub parse_commands {
	my ($content) = @_;
	my $parser = ShellParser->new(\$content);
	my @all_tokens = $parser->parse();

	my @commands;
	my @current;
	my $lineno = 1;

	for (my $ti = 0; $ti < @all_tokens; $ti++) {
		my $text = $all_tokens[$ti]->[0];
		if ($text =~ /^(?:&&|\|\||;;|\n)$/) {
			if (@current) {
				push @commands, {
					tokens => [@current],
					lineno => $lineno,
				};
				@current = ();
			}
		} else {
			$lineno = $all_tokens[$ti]->[3]
				if !@current && defined $all_tokens[$ti]->[3];
			push @current, $text;
		}
	}
	if (@current) {
		push @commands, {
			tokens => [@current],
			lineno => $lineno,
		};
	}
	return @commands;
}

# --- Rule: '! test_grep' should be 'test_grep !' ---
# Shell-level negation suppresses test_grep's diagnostic output
# on failure.  Built-in negation preserves it.
sub check_test_grep_negation {
	my ($cmd, $file, $line_ref) = @_;
	my @tokens = @{$cmd->{tokens}};
	return unless @tokens >= 2 && $tokens[0] eq '!' && $tokens[1] eq 'test_grep';

	return report_violation($file, $cmd, $line_ref,
		qr/!\s*test_grep/, 'test_grep !', '! test_grep');
}

# Map parsed commands back to raw file lines for --fix.
# Detection uses parsed tokens (correct handling of quoting,
# heredocs, pipes) but fixes must modify the original text
# to preserve formatting.
package LintParser;

sub check_test {
	# Called by ScriptParser::parse_cmd for each test_expect_success
	# or test_expect_failure block.
	my $self = shift @_;
	my $title = ScriptParser::unwrap(shift @_);

	# Two test body formats:
	#   Quoted:  test_expect_success 'title' '..body..'
	#   Heredoc: test_expect_success 'title' - <<\EOF
	#              ..body..
	#            EOF
	# For quoted, the body token is the quoted string.
	# For heredoc, the body token is '-' and the actual
	# code arrives as the next argument from the Lexer.
	my $body_token = shift @_;
	my $lineno_base = $body_token->[3] || 1;
	my $body = ScriptParser::unwrap($body_token);

	if ($body eq '-') {
		my $herebody = shift @_;
		if ($herebody) {
			$body = $herebody->{content};
			$lineno_base = $herebody->{start_line} || 1;
		}
	}
	return unless $body;

	# Map each command back to its file line number.
	# $lineno_base is where the body starts in the file;
	# $cmd->{lineno} is relative to the body (starting at 1).
	my $raw_lines = $self->{raw_lines};
	for my $cmd (main::parse_commands($body)) {
		my $ln = ($cmd->{lineno} || 0) + $lineno_base - 1;
		$cmd->{lineno} = $ln;
		next unless $ln >= 1 && $ln <= @$raw_lines;
		next if $raw_lines->[$ln - 1] =~ /#.*lint-ok/;

		if (main::check_test_grep_negation($cmd, $self->{file}, \$raw_lines->[$ln - 1])) {
			$self->{dirty} = 1;
		}
	}
}

package main;

for my $file (@ARGV) {
	# :unix:crlf strips \r on Windows (same as chainlint.pl)
	open(my $fh, '<:unix:crlf', $file) or die "$0: $file: $!\n";
	my @raw_lines = <$fh>;
	close $fh;

	my $parser = LintParser->new(\join('', @raw_lines));
	$parser->{file} = $file;
	$parser->{raw_lines} = \@raw_lines;
	$parser->{dirty} = 0;
	$parser->parse();

	if ($fix_mode && $parser->{dirty}) {
		open(my $out, '>', $file) or die "$0: $file: $!\n";
		print $out @{$parser->{raw_lines}};
		close $out;
	}
}

if ($has_fixable && !$fix_mode) {
	print "hint: run with --fix to apply the suggested replacements.\n";
}
exit $exit_code;
