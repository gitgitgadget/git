#!/bin/sh

test_description='Test git config-batch'

. ./test-lib.sh

# usage: test_zformat <command> <args> <in >out
#
# Let 'in' be a z-format input but with " NUL " between tokens in
# a single command and " NUL NUL" trailing each line.
#
# The values in 'out' will be space- and newline-delimited where
# NUL-bytes would normally be output.
test_zformat () {
	sed -e "s/\ NUL\ /!/g" >nullin1 &&
	sed -e "s/NUL//g" <nullin1 >nullin2 &&

	tr "!" "\0" <nullin2 >nullin3 &&
	tr "\n" "\0" <nullin3 >zin &&

	$* <zin >zout &&

	tr "\0" " " <zout >outspace &&
	sed "s/\ \ /\n/g" <outspace
}

test_expect_success 'no commands' '
	echo | git config-batch >out &&
	test_must_be_empty out
'

test_expect_success 'unknown_command' '
	echo unknown_command >expect &&
	echo "bogus 1 line of tokens" >in &&
	git config-batch >out <in &&
	test_cmp expect out
'

test_expect_success 'completely broken input' '
	echo "not_even_two_tokens" >in &&
	test_must_fail git config-batch 2>err <in &&
	test_grep "expected at least 2 tokens" err &&
	test_grep "an unrecoverable error occurred during command execution" err
'

test_expect_success 'help command' '
	echo "help 1" >in &&

	cat >expect <<-\EOF &&
	help 1 count 3
	help 1 help 1
	help 1 get 1
	help 1 set 1
	EOF

	git config-batch >out <in &&
	test_cmp expect out
'

test_expect_success 'help -z' '
	cat >in <<-\EOF &&
	4:help NUL 1:1 NUL NUL
	5:bogus NUL 2:10 NUL NUL
	EOF

	cat >expect <<-\EOF &&
	4:help 1:1 5:count 1:3
	4:help 1:1 4:help 1:1
	4:help 1:1 3:get 1:1
	4:help 1:1 3:set 1:1
	15:unknown_command
	EOF

	test_zformat git config-batch -z >out <in &&
	test_cmp expect out
'

test_expect_success 'failed to parse version' '
	echo "bogus BAD_VERSION line of tokens" >in &&
	test_must_fail git config-batch 2>err <in &&
	test_grep BAD_VERSION err
'

test_expect_success 'get inherited config' '
	test_when_finished git config --unset test.key &&

	git config test.key "test value with spaces" &&

	echo "get 1 inherited test.key" >in &&
	echo "get 1 found test.key local test value with spaces" >expect &&
	git config-batch >out <in &&
	test_cmp expect out &&

	echo "get 1 global test.key" >in &&
	echo "get 1 missing test.key" >expect &&
	git config-batch >out <in &&
	test_cmp expect out
'

test_expect_success 'set up worktree' '
	test_commit A &&
	git config extensions.worktreeconfig true &&
	git worktree add --detach worktree
'

test_expect_success 'get config with arg:regex' '
	test_when_finished git config --unset-all test.key &&
	GIT_CONFIG_SYSTEM=system-config-file &&
	GIT_CONFIG_NOSYSTEM=0 &&
	GIT_CONFIG_GLOBAL=global-config-file &&
	export GIT_CONFIG_SYSTEM &&
	export GIT_CONFIG_NOSYSTEM &&
	export GIT_CONFIG_GLOBAL &&

	git config --system test.key on1e &&
	git config --global test.key t2wo &&
	git config test.key "thre3e space" &&
	git config --worktree test.key 4four &&

	cat >in <<-\EOF &&
	get 1 inherited test.key arg:regex .*1.*
	get 1 inherited test.key arg:regex [a-z]2.*
	get 1 inherited test.key arg:regex .*3e s.*
	get 1 inherited test.key arg:regex 4.*
	get 1 inherited test.key arg:regex .*5.*
	get 1 inherited test.key arg:regex .*6.*
	EOF

	cat >expect <<-\EOF &&
	get 1 found test.key system on1e
	get 1 found test.key global t2wo
	get 1 found test.key local thre3e space
	get 1 found test.key worktree 4four
	get 1 found test.key command five5
	get 1 missing test.key .*6.*
	EOF

	git -c test.key=five5 config-batch >out <in &&
	test_cmp expect out
'

test_expect_success 'get config with arg:fixed-value' '
	test_when_finished git config --unset-all test.key &&
	GIT_CONFIG_SYSTEM=system-config-file &&
	GIT_CONFIG_NOSYSTEM=0 &&
	GIT_CONFIG_GLOBAL=global-config-file &&
	export GIT_CONFIG_SYSTEM &&
	export GIT_CONFIG_NOSYSTEM &&
	export GIT_CONFIG_GLOBAL &&

	git config --system test.key one &&
	git config --global test.key two &&
	git config test.key "three space" &&
	git config --worktree test.key four &&

	cat >in <<-\EOF &&
	get 1 inherited test.key arg:fixed-value one
	get 1 inherited test.key arg:fixed-value two
	get 1 inherited test.key arg:fixed-value three space
	get 1 inherited test.key arg:fixed-value four
	get 1 inherited test.key arg:fixed-value five
	get 1 inherited test.key arg:fixed-value six
	EOF

	cat >expect <<-\EOF &&
	get 1 found test.key system one
	get 1 found test.key global two
	get 1 found test.key local three space
	get 1 found test.key worktree four
	get 1 found test.key command five
	get 1 missing test.key six
	EOF

	git -c test.key=five config-batch >out <in &&
	test_cmp expect out
'

test_expect_success 'get config with -z' '
	test_when_finished git config --unset-all test.key &&
	GIT_CONFIG_SYSTEM=system-config-file &&
	GIT_CONFIG_NOSYSTEM=0 &&
	GIT_CONFIG_GLOBAL=global-config-file &&
	export GIT_CONFIG_SYSTEM &&
	export GIT_CONFIG_NOSYSTEM &&
	export GIT_CONFIG_GLOBAL &&

	git config --system test.key on1e &&
	git config --global test.key t2wo &&
	git config test.key "thre3e space" &&
	git config --worktree test.key 4four &&

	cat >in <<-\EOF &&
	3:get NUL 1:1 NUL 9:inherited NUL 8:test.key NUL NUL
	3:get NUL 1:1 NUL 6:global NUL 8:test.key NUL 9:arg:regex NUL 3:2.* NUL NUL
	3:get NUL 1:1 NUL 5:local NUL 8:test.key NUL 15:arg:fixed-value NUL 12:thre3e space NUL NUL
	3:get NUL 1:1 NUL 9:inherited NUL 11:key.missing NUL NUL
	EOF

	cat >expect <<-\EOF &&
	3:get 1:1 5:found 8:test.key 8:worktree 5:4four
	3:get 1:1 5:found 8:test.key 6:global 4:t2wo
	3:get 1:1 5:found 8:test.key 5:local 12:thre3e space
	3:get 1:1 7:missing 11:key.missing
	EOF

	test_zformat git config-batch -z >out <in &&
	test_cmp expect out
'

test_expect_success 'set config by scope' '
	test_when_finished git config remove-section test.set &&
	GIT_CONFIG_SYSTEM=system-config-file &&
	GIT_CONFIG_NOSYSTEM=0 &&
	GIT_CONFIG_GLOBAL=global-config-file &&
	export GIT_CONFIG_SYSTEM &&
	export GIT_CONFIG_NOSYSTEM &&
	export GIT_CONFIG_GLOBAL &&

	cat >in <<-\EOF &&
	set 1 system test.set.system system
	set 1 global test.set.global global
	set 1 local test.set.local local with spaces
	set 1 worktree test.set.worktree worktree
	set 1 submodule test.set.submodule submodule
	set 1 command test.set.command command
	set 1 inherited test.set.inherited inherited
	EOF

	cat >expect <<-\EOF &&
	set 1 success system test.set.system system
	set 1 success global test.set.global global
	set 1 success local test.set.local local with spaces
	set 1 success worktree test.set.worktree worktree
	command_parse_error set
	command_parse_error set
	command_parse_error set
	EOF

	git config-batch <in >out 2>err &&

	test_must_be_empty err &&
	test_cmp expect out &&

	cat >expect-values <<-EOF &&
	file:system-config-file	system
	file:global-config-file	global
	file:.git/config	local with spaces
	file:.git/config.worktree	worktree
	EOF

	git config get --show-origin --regexp --all test.set.* >values &&
	test_cmp expect-values values
'

test_expect_success 'set config by scope with -z' '
	test_when_finished git config remove-section test.set &&
	GIT_CONFIG_SYSTEM=system-config-file &&
	GIT_CONFIG_NOSYSTEM=0 &&
	GIT_CONFIG_GLOBAL=global-config-file &&
	export GIT_CONFIG_SYSTEM &&
	export GIT_CONFIG_NOSYSTEM &&
	export GIT_CONFIG_GLOBAL &&

	cat >in <<-\EOF &&
	3:set NUL 1:1 NUL 6:system NUL 15:test.set.system NUL 6:system NUL NUL
	3:set NUL 1:1 NUL 6:global NUL 15:test.set.global NUL 6:global NUL NUL
	3:set NUL 1:1 NUL 5:local NUL 14:test.set.local NUL 17:local with spaces NUL NUL
	3:set NUL 1:1 NUL 8:worktree NUL 17:test.set.worktree NUL 8:worktree NUL NUL
	3:set NUL 1:1 NUL 9:submodule NUL 18:test.set.submodule NUL 9:submodule NUL NUL
	3:set NUL 1:1 NUL 7:command NUL 16:test.set.command NUL 7:command NUL NUL
	3:set NUL 1:1 NUL 9:inherited NUL 18:test.set.inherited NUL 9:inherited NUL NUL
	EOF

	cat >expect <<-\EOF &&
	3:set 1:1 7:success 6:system 15:test.set.system 6:system
	3:set 1:1 7:success 6:global 15:test.set.global 6:global
	3:set 1:1 7:success 5:local 14:test.set.local 17:local with spaces
	3:set 1:1 7:success 8:worktree 17:test.set.worktree 8:worktree
	19:command_parse_error 3:set
	19:command_parse_error 3:set
	19:command_parse_error 3:set
	EOF

	test_zformat git config-batch -z >out <in &&
	test_cmp expect out &&

	cat >expect-values <<-EOF &&
	file:system-config-file	system
	file:global-config-file	global
	file:.git/config	local with spaces
	file:.git/config.worktree	worktree
	EOF

	git config get --show-origin --regexp --all test.set.* >values &&
	test_cmp expect-values values
'

test_done
