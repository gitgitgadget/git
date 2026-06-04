#!/usr/bin/perl

# Check test scripts for style violations that can be detected
# mechanically, such as using bare 'grep' where test_grep should
# be used.  Use --fix to automatically apply suggested replacements.
#
# Detection uses parsed tokens from the shared shell parser for
# correct handling of heredocs, $(...), pipes, and quoting.
# Fixes modify the original file text to preserve formatting.
#
# Architecture: the harness (LintParser, parse_commands) tokenizes
# test bodies and splits them into commands.  Rules are independent
# functions that examine each command and its surrounding token
# context to decide if there is a violation.

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
# Subroutines defined below are in package main and called with
# the main:: prefix.  File-scoped lexicals ($fix_mode, etc.) are
# visible across packages since 'package' does not introduce a
# new scope.
package LintParser;
our @ISA = ('ScriptParser');

package main;

my $exit_code = 0;
my $has_fixable = 0;

my %skip_file = map { $_ => 1 }
	grep { m{(?:test-lib-functions|lib-rebase)\.sh$} } @ARGV;

sub err {
	my ($file, $lineno, $line, $msg, %opts) = @_;
	$line =~ s/^\s+//;
	$line =~ s/\s+$//;
	$line =~ s/\s+/ /g;
	my $prefix = ($fix_mode && $opts{fixable}) ? 'fixed' : 'error';
	print "$file:$lineno: $prefix: $msg: $line\n";
	$exit_code = 1 unless $fix_mode && $opts{fixable};
}

# Report a lint violation.  In --fix mode, apply the regex
# substitution on the raw line.  Returns 1 if modified.
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

# --- Harness: tokenize and split into commands ---
#
# Split a token stream into commands at &&, ||, ;;, and \n.
# Each command is {tokens => [...], lineno => N, token_pos => I}
# where token_pos is the index in @all_tokens where the command's
# first token appeared (so rules can look backward for context).
sub parse_commands {
	my ($all_tokens) = @_;
	my @commands;
	my @current;
	my $lineno = 1;
	my $first_pos = 0;

	my %shell_keyword;
	@shell_keyword{qw(if then else elif fi for do done
			   while until case in esac)} = ();

	for (my $ti = 0; $ti < @$all_tokens; $ti++) {
		my $text = $all_tokens->[$ti]->[0];
		if ($text =~ /^(?:&&|\|\||;;|\n)$/) {
			# Command separators: flush current command
			if (@current) {
				push @commands, {
					tokens    => [@current],
					lineno    => $lineno,
					token_pos => $first_pos,
				};
				@current = ();
			}
		} elsif ($text =~ /^[{}()|]$/ || exists $shell_keyword{$text}) {
			# Shell structural tokens and keywords:
			# flush current command (these are boundaries,
			# not part of the command's arguments)
			if (@current) {
				push @commands, {
					tokens    => [@current],
					lineno    => $lineno,
					token_pos => $first_pos,
				};
				@current = ();
			}
		} else {
			if (!@current) {
				# Record line number of the first token
				$lineno = $all_tokens->[$ti]->[3]
					if defined $all_tokens->[$ti]->[3];
				$first_pos = $ti;
			}
			push @current, $text;
		}
	}
	if (@current) {
		push @commands, {
			tokens    => [@current],
			lineno    => $lineno,
			token_pos => $first_pos,
		};
	}
	return @commands;
}

# --- Rule: '! test_grep' should be 'test_grep !' ---
sub check_test_grep_negation {
	my ($cmd, $file, $line_ref, $all_tokens) = @_;
	my @tokens = @{$cmd->{tokens}};
	return unless @tokens >= 2 && $tokens[0] eq '!' && $tokens[1] eq 'test_grep';

	return report_violation($file, $cmd, $line_ref,
		qr/!\s*test_grep/, 'test_grep !', '! test_grep');
}

# --- Rule: bare 'grep' should be 'test_grep' ---

# Check if this command is in a filter context by looking at
# the surrounding tokens in the stream.  This is grep-rule
# specific: it knows what contexts make a grep not an assertion.
sub is_filter_context {
	my ($all_tokens, $cmd) = @_;
	my $pos = $cmd->{token_pos};

	# Scan backward to the previous command separator.
	# If we find '|', this command is part of a pipeline.
	# If we find if/elif/while/until, it's a condition.
	for (my $j = $pos - 1; $j >= 0; $j--) {
		my $t = $all_tokens->[$j]->[0];
		# Stop at command separators (but not \n after |)
		last if $t =~ /^(?:&&|\|\||;;)$/;
		if ($t eq "\n") {
			# \n after | is a line continuation, keep scanning
			next if $j > 0 && $all_tokens->[$j - 1]->[0] eq '|';
			last;
		}
		return 1 if $t eq '|';
		return 1 if $t =~ /^(?:if|elif|while|until)$/;
		# for ... in ITEMS ... do: if we're between 'in' and 'do',
		# we're in a value list, not a command
		return 1 if $t eq 'in';
	}

	# Forward: pipe after command
	for (my $j = $pos + @{$cmd->{tokens}}; $j < @$all_tokens; $j++) {
		my $t = $all_tokens->[$j]->[0];
		last if $t =~ /^(?:&&|\|\||;;|\n)$/;
		return 1 if $t eq '|';
	}

	# { cmd; } >output
	return 1 if is_in_redirected_brace($all_tokens, $pos);

	return 0;
}

# Check if position $pos is inside a brace group whose output is
# redirected: { grep ...; } >file.  Scan backward for the enclosing
# '{', then forward for the matching '}', and check what follows it.
sub is_in_redirected_brace {
	my ($all_tokens, $pos) = @_;
	my $brace_depth = 0;
	for (my $j = $pos - 1; $j >= 0; $j--) {
		my $t = $all_tokens->[$j]->[0];
		$brace_depth++ if $t eq '}';
		if ($t eq '{') {
			return 0 if $brace_depth > 0;
			$brace_depth--;
			# Found our enclosing '{'. Find matching '}'
			my $depth = 1;
			for (my $k = $j + 1; $k < @$all_tokens; $k++) {
				$depth++ if $all_tokens->[$k]->[0] eq '{';
				$depth-- if $all_tokens->[$k]->[0] eq '}';
				if ($depth == 0) {
					my $after = $k + 1 < @$all_tokens ?
						$all_tokens->[$k + 1]->[0] : '';
					return $after =~ /^>{1,2}/;
				}
			}
			return 0;
		}
	}
	return 0;
}

# Classify a grep command: assertion, filter, or bug.
#
# Returns:
#   1  assertion (PATTERN + FILE), can be converted to test_grep
#   0  not a grep, or grep used as a filter
#  -1  likely bug (e.g., missing file argument)
sub is_grep_assertion {
	my ($cmd, $all_tokens) = @_;
	my @tokens = @{$cmd->{tokens}};

	# Find grep, possibly after "!"
	my $i = 0;
	$i++ if $tokens[0] eq '!';
	return 0 unless defined $tokens[$i] && $tokens[$i] eq 'grep';
	return 0 if grep { $_ eq 'test_grep' } @tokens;

	# Check surrounding context (pipes, control flow, brace redirects)
	return 0 if is_filter_context($all_tokens, $cmd);

	$i++;  # skip 'grep'

	# Check grep's own flags and arguments
	my @positional;
	my $has_pattern_flag = 0;
	my $end_of_flags = 0;
	while ($i < @tokens) {
		my $tok = $tokens[$i];
		if ($tok eq '|' || $tok eq '<') {
			return 0;
		}
		if ($tok =~ /^>{1,2}$/) {
			# Stdout redirect means filter (grep ... >out).
			# Stderr redirect (2>err) is fine: skip the fd
			# and the target, and keep classifying.
			my $prev = $i > 0 ? $tokens[$i - 1] : '';
			return 0 unless $prev =~ /^\d+$/ && $prev >= 2;
			pop @positional if @positional && $positional[-1] eq $prev;
			$i += 2;
			next;
		}
		if (!$end_of_flags && $tok =~ /^-\w*[clLrR]/) {
			return 0;
		}
		if (!$end_of_flags && $tok eq '--') {
			$end_of_flags = 1;
		} elsif (!$end_of_flags && $tok =~ /^-\w*[ef]$/) {
			$has_pattern_flag = 1;
			$i++;
		} elsif (!$end_of_flags && $tok =~ /^-/) {
			# skip other flags
		} else {
			push @positional, $tok;
		}
		$i++;
	}

	my $need = $has_pattern_flag ? 1 : 2;
	return 0 if !@positional && !$has_pattern_flag;
	return -1 if @positional < $need;
	return 0 if $positional[-1] =~ /^-/;
	return 1;
}

sub check_bare_grep {
	my ($cmd, $file, $line_ref, $all_tokens) = @_;
	my @tokens = @{$cmd->{tokens}};

	my $result = is_grep_assertion($cmd, $all_tokens);
	return unless $result;

	if ($result == -1) {
		err $file, $cmd->{lineno}, join(' ', @tokens),
			"grep assertion appears to be missing a file argument";
		return 0;
	}

	# Determine negation and -q flag
	my $negated = $tokens[0] eq '!';
	my $has_q = 0;
	my ($pre_q, $post_q) = ('', '');
	for my $tok (@tokens) {
		if ($tok =~ /^-(\w*)q(\w*)$/) {
			$has_q = 1;
			($pre_q, $post_q) = ($1, $2);
			last;
		}
		last if $tok !~ /^-/ && $tok ne '!' && $tok ne 'grep';
	}

	# Build the replacement
	my $fix = "test_grep";
	$fix .= " !" if $negated;
	if ($has_q) {
		my $rest = "$pre_q$post_q";
		$fix .= " -$rest" if $rest;
	}

	# Build the match pattern
	my $neg_match = $negated ? '!\s*' : '\b';
	my $neg_from  = $negated ? '! '   : '';
	my ($match, $from);
	if ($has_q) {
		$match = qr/${neg_match}grep\s+-\w*q\w*/;
		$from  = "${neg_from}grep -${pre_q}q${post_q}";
	} else {
		$match = qr/${neg_match}grep\b/;
		$from  = "${neg_from}grep";
	}

	return report_violation($file, $cmd, $line_ref,
		$match, $fix, $from);
}

# --- Harness: LintParser.check_test ---
#
# Called by ScriptParser::parse_cmd for each test_expect_success
# or test_expect_failure block.  Extracts the body, tokenizes it,
# splits into commands, and runs each rule.
package LintParser;

sub check_test {
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

	# Tokenize the body once; commands and rules share the stream
	my $parser = ShellParser->new(\$body);
	my @all_tokens = $parser->parse();
	my @commands = main::parse_commands(\@all_tokens);

	# Map each command back to its file line number.
	# $lineno_base is where the body starts in the file;
	# $cmd->{lineno} is relative to the body (starting at 1).
	my $raw_lines = $self->{raw_lines};
	for my $cmd (@commands) {
		my $ln = ($cmd->{lineno} || 0) + $lineno_base - 1;
		$cmd->{lineno} = $ln;
		next unless $ln >= 1 && $ln <= @$raw_lines;
		next if $raw_lines->[$ln - 1] =~ /#.*lint-ok/;

		my $line_ref = \$raw_lines->[$ln - 1];
		# Stop after the first fix: later rules should not
		# re-match against already-modified text.
		my $modified = 0;
		$modified ||= main::check_test_grep_negation(
			$cmd, $self->{file}, $line_ref, \@all_tokens);
		$modified ||= main::check_bare_grep(
			$cmd, $self->{file}, $line_ref, \@all_tokens);
		$self->{dirty} = 1 if $modified;
	}
}

package main;

for my $file (@ARGV) {
	next if $skip_file{$file};
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
