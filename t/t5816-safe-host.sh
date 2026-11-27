#!/bin/sh

test_description='safe.host restricts the remote hosts Git connects to'

. ./test-lib.sh

test_expect_success 'setup' '
	test_commit one &&
	git init --bare repo.git &&
	git push repo.git HEAD &&

	write_script ssh-wrapper <<-\EOF &&
	echo "$*" >>"$TRASH_DIRECTORY/ssh-log"
	# Ignore the host and port; the command is the last argument.
	for cmd
	do
		:
	done
	cd "$TRASH_DIRECTORY" &&
	eval "$cmd"
	EOF
	GIT_SSH="$PWD/ssh-wrapper" &&
	GIT_SSH_VARIANT=ssh &&
	export GIT_SSH GIT_SSH_VARIANT TRASH_DIRECTORY
'

# Usage: check_safe_hosts (allow|deny) <url> [<safe.host entry>...]
#
# Configure each given entry in the global config, and then check that
# "git ls-remote <url>" is allowed to connect, or is denied before it
# even tries to.
check_safe_hosts () {
	expect=$1 &&
	url=$2 &&
	shift 2 &&
	test_when_finished "git config --global --unset-all safe.host || :" &&
	{ git config --global --unset-all safe.host || :; } &&
	for entry in "$@"
	do
		git config --global --add safe.host "$entry" || return 1
	done &&
	rm -f ssh-log &&
	case "$expect" in
	allow)
		git ls-remote "$url" >out &&
		test_grep refs/heads/ out
		;;
	deny)
		test_must_fail git ls-remote "$url" 2>err &&
		test_grep "is not allowed by safe.host" err &&
		test_path_is_missing ssh-log
		;;
	esac
}

test_expect_success 'all hosts are allowed when safe.host is unset' '
	check_safe_hosts allow "ssh://host.example.com$PWD/repo.git"
'

test_expect_success 'listed host is allowed' '
	check_safe_hosts allow "ssh://host.example.com$PWD/repo.git" \
		other.example.com host.example.com
'

test_expect_success 'unlisted host is denied' '
	check_safe_hosts deny "ssh://evil.example.com$PWD/repo.git" \
		host.example.com
'

test_expect_success 'denial names the connection and suggests how to allow it' '
	test_when_finished "git config --global --unset-all safe.host" &&
	git config --global safe.host host.example.com &&
	test_must_fail git clone "ssh://evil.example.com$PWD/repo.git" 2>err &&
	test_grep "connecting to .ssh://evil.example.com. is not allowed by safe.host" err &&
	test_grep "git config --global --add safe.host ssh://evil.example.com$" err &&
	test_must_fail git ls-remote "evil.example.com:repo.git" 2>err &&
	test_grep "git config --global --add safe.host ssh://evil.example.com$" err &&
	test_must_fail git ls-remote "ssh://evil.example.com:22$PWD/repo.git" 2>err &&
	test_grep "git config --global --add safe.host ssh://evil.example.com$" err &&
	test_must_fail git ls-remote "ssh://evil.example.com:2222$PWD/repo.git" 2>err &&
	test_grep "git config --global --add safe.host ssh://evil.example.com:2222$" err &&
	test_must_fail git ls-remote "ssh://[::2]$PWD/repo.git" 2>err &&
	test_grep "git config --global --add safe.host .ssh://\[::2\].$" err
'

test_expect_success 'denial names the scheme when only other schemes are allowed' '
	test_must_fail git -c safe.host=git://host.example.com \
		ls-remote "ssh://host.example.com$PWD/repo.git" 2>err &&
	test_grep "connecting to .ssh://host.example.com. is not allowed by safe.host" err
'

test_expect_success 'hosts are matched case-insensitively' '
	check_safe_hosts allow "ssh://Host.Example.COM$PWD/repo.git" \
		HOST.example.com
'

test_expect_success 'scp-like syntax is matched as ssh' '
	check_safe_hosts allow "git@host.example.com:repo.git" \
		ssh://host.example.com:22 &&
	check_safe_hosts deny "git@evil.example.com:repo.git" \
		host.example.com
'

test_expect_success 'scheme limits the entry to that scheme' '
	check_safe_hosts allow "ssh://host.example.com$PWD/repo.git" \
		ssh://host.example.com &&
	check_safe_hosts deny "ssh://host.example.com$PWD/repo.git" \
		https://host.example.com &&
	check_safe_hosts deny "git://host.example.com/repo.git" \
		ssh://host.example.com
'

test_expect_success 'entry without a port matches any port' '
	check_safe_hosts allow "ssh://host.example.com:2222$PWD/repo.git" \
		host.example.com &&
	check_safe_hosts allow "[host.example.com:2222]:repo.git" \
		host.example.com
'

test_expect_success 'entry with a port matches only that port' '
	check_safe_hosts allow "ssh://host.example.com:2222$PWD/repo.git" \
		host.example.com:2222 &&
	check_safe_hosts allow "[host.example.com:2222]:repo.git" \
		host.example.com:2222 &&
	check_safe_hosts deny "ssh://host.example.com$PWD/repo.git" \
		host.example.com:2222 &&
	check_safe_hosts allow "ssh://host.example.com$PWD/repo.git" \
		host.example.com:22 &&
	check_safe_hosts deny "git://host.example.com:9419/repo.git" \
		host.example.com:9418
'

test_expect_success 'wildcard matches exactly one host component' '
	check_safe_hosts allow "ssh://git.example.com$PWD/repo.git" \
		"*.example.com" &&
	check_safe_hosts deny "ssh://example.com$PWD/repo.git" \
		"*.example.com" &&
	check_safe_hosts deny "ssh://a.b.example.com$PWD/repo.git" \
		"*.example.com"
'

test_expect_success 'wildcard host matches any host' '
	check_safe_hosts allow "ssh://host.example.com$PWD/repo.git" \
		"ssh://*" &&
	check_safe_hosts deny "ssh://host.example.com$PWD/repo.git" \
		"https://*"
'

test_expect_success 'IPv6 literals must be bracketed' '
	check_safe_hosts allow "ssh://[::1]$PWD/repo.git" "[::1]" &&
	check_safe_hosts allow "ssh://[::1]:2222$PWD/repo.git" "[::1]:2222" &&
	check_safe_hosts deny "ssh://[::1]$PWD/repo.git" "[::2]"
'

test_expect_success 'ssh destinations that look like URIs are denied' '
	check_safe_hosts deny "ssh://ssh://u@[host.example.com:2222]$PWD/repo.git" \
		"*:22" &&
	check_safe_hosts deny "ssh://[ssh://u@host.example.com:2222]$PWD/repo.git" \
		"*:22"
'

test_expect_success 'invalid entries are ignored with a warning' '
	test_when_finished "git config --global --unset-all safe.host" &&
	for entry in host.example.com/repo.git user@host.example.com \
		host.example.com: host.example.com:0 host.example.com:65536 \
		"[::1" ::1 "://host.example.com"
	do
		git config --global --add safe.host "$entry" || return 1
	done &&
	test_must_fail git ls-remote "ssh://host.example.com$PWD/repo.git" 2>err &&
	test_grep "is not allowed by safe.host" err &&
	for entry in host.example.com/repo.git user@host.example.com \
		host.example.com: host.example.com:0 host.example.com:65536 \
		"[::1" ::1 "://host.example.com"
	do
		test_grep -F "ignoring invalid safe.host entry $SQ$entry$SQ" err ||
		return 1
	done
'

test_expect_success 'trailing slash is tolerated' '
	check_safe_hosts allow "ssh://host.example.com$PWD/repo.git" \
		ssh://host.example.com/
'

test_expect_success '"*" allows all hosts' '
	check_safe_hosts allow "ssh://host.example.com$PWD/repo.git" \
		other.example.com "*"
'

test_expect_success 'empty value resets the list' '
	check_safe_hosts deny "ssh://host.example.com$PWD/repo.git" \
		"*" host.example.com "" &&
	check_safe_hosts allow "ssh://host.example.com$PWD/repo.git" \
		"*" "" host.example.com &&
	check_safe_hosts deny "ssh://other.example.com$PWD/repo.git" \
		"*" "" host.example.com
'

test_expect_success 'safe.host is respected on the command line' '
	test_must_fail git -c safe.host=host.example.com \
		ls-remote "ssh://evil.example.com$PWD/repo.git" 2>err &&
	test_grep "is not allowed by safe.host" err
'

test_expect_success 'safe.host is ignored in repository config' '
	test_config safe.host other.example.com &&
	check_safe_hosts allow "ssh://host.example.com$PWD/repo.git" &&
	check_safe_hosts deny "ssh://other.example.com$PWD/repo.git" \
		host.example.com
'

test_expect_success 'local repositories are not affected' '
	check_safe_hosts allow repo.git "" &&
	check_safe_hosts allow "file://$PWD/repo.git" ""
'

test_done
