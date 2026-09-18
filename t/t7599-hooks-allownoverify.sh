#!/bin/sh

test_description='support hook.allowNoVerify configuration to disallow --no-verify'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

test_expect_success 'setup test repository and hooks' '
	test_commit init &&
	test_hook --setup pre-commit <<-\HOOK_EOF &&
	echo "pre-commit executed" >>pre-commit.log
	if test -f fail-pre-commit
	then
		exit 1
	fi
	exit 0
	HOOK_EOF
	test_hook --setup pre-push <<-\HOOK_EOF &&
	echo "pre-push executed" >>pre-push.log
	if test -f fail-pre-push
	then
		exit 1
	fi
	exit 0
	HOOK_EOF
	git init --bare remote.git &&
	git remote add origin remote.git &&
	git push -u origin main &&
	rm -f pre-commit.log pre-push.log
'

test_expect_success 'default: --no-verify is permitted for git commit' '
	test_when_finished "rm -f pre-commit.log" &&
	echo "change1" >>init.t &&
	git add init.t &&
	git commit --no-verify -m "commit with no-verify (default)" &&
	test_path_is_missing pre-commit.log
'

test_expect_success 'default: -n is permitted for git commit' '
	test_when_finished "rm -f pre-commit.log" &&
	echo "change2" >>init.t &&
	git add init.t &&
	git commit -n -m "commit with -n (default)" &&
	test_path_is_missing pre-commit.log
'

test_expect_success 'default: --no-verify is permitted for git push' '
	test_when_finished "rm -f pre-push.log" &&
	rm -f pre-push.log &&
	git push --no-verify origin main &&
	test_path_is_missing pre-push.log
'

test_expect_success 'explicit hook.allowNoVerify=true allows --no-verify' '
	test_when_finished "rm -f pre-commit.log" &&
	test_config hook.allowNoVerify true &&
	echo "change3" >>init.t &&
	git add init.t &&
	git commit --no-verify -m "commit with no-verify allowed" &&
	test_path_is_missing pre-commit.log
'

test_expect_success 'hook.allowNoVerify=false disallows git commit --no-verify' '
	test_config hook.allowNoVerify false &&
	echo "change4" >>init.t &&
	git add init.t &&
	test_must_fail git commit --no-verify -m "should fail" 2>err &&
	test_grep "hook.allowNoVerify" err
'

test_expect_success 'hook.allowNoVerify=false disallows git commit -n' '
	test_config hook.allowNoVerify false &&
	echo "change5" >>init.t &&
	git add init.t &&
	test_must_fail git commit -n -m "should fail" 2>err &&
	test_grep "hook.allowNoVerify" err
'

test_expect_success 'hook.allowNoVerify=false disallows git push --no-verify' '
	test_config hook.allowNoVerify false &&
	test_must_fail git push --no-verify origin main 2>err &&
	test_grep "hook.allowNoVerify" err
'

test_expect_success 'hook.allowNoVerify=false disallows git merge --no-verify' '
	test_config hook.allowNoVerify false &&
	git checkout -b branch-merge main &&
	echo "merge change" >merge_file &&
	git add merge_file &&
	git commit -m "merge commit" &&
	git checkout main &&
	test_must_fail git merge --no-verify branch-merge -m "merge fail" 2>err &&
	test_grep "hook.allowNoVerify" err
'

test_expect_success 'hook.allowNoVerify=false disallows git rebase --no-verify' '
	test_config hook.allowNoVerify false &&
	test_must_fail git rebase --no-verify main branch-merge 2>err &&
	test_grep "hook.allowNoVerify" err
'

test_expect_success 'hook.allowNoVerify=false disallows git am --no-verify' '
	test_when_finished "rm -f patch && git am --abort || true" &&
	test_config hook.allowNoVerify false &&
	git format-patch -1 --stdout branch-merge >patch &&
	test_must_fail git am --no-verify patch 2>err &&
	test_grep "hook.allowNoVerify" err
'

test_expect_success 'hook.allowNoVerify=false still runs hooks when --no-verify is not used' '
	test_when_finished "rm -f pre-commit.log" &&
	test_config hook.allowNoVerify false &&
	echo "change6" >>init.t &&
	git add init.t &&
	git commit -m "normal commit" &&
	test_path_is_file pre-commit.log
'

test_expect_success 'hook.allowNoVerify=false enforces hook execution (hook failure prevents commit)' '
	test_when_finished "rm -f fail-pre-commit pre-commit.log" &&
	test_config hook.allowNoVerify false &&
	touch fail-pre-commit &&
	echo "change7" >>init.t &&
	git add init.t &&
	test_must_fail git commit -m "failing hook" &&
	test_must_fail git commit --no-verify -m "cannot bypass" 2>err &&
	test_grep "hook.allowNoVerify" err
'

test_expect_success 'hook.allowNoVerify=false still runs pre-push hook on git push' '
	test_when_finished "rm -f pre-push.log" &&
	test_config hook.allowNoVerify false &&
	git push origin main &&
	test_path_is_file pre-push.log
'

test_expect_success 'CLI -c hook.allowNoVerify=false overrides local true' '
	test_config hook.allowNoVerify true &&
	echo "change8" >>init.t &&
	git add init.t &&
	test_must_fail git -c hook.allowNoVerify=false commit --no-verify -m "override" 2>err &&
	test_grep "hook.allowNoVerify" err
'

test_expect_success 'local hook.allowNoVerify=false overrides global true' '
	test_config_global hook.allowNoVerify true &&
	test_config hook.allowNoVerify false &&
	echo "change9" >>init.t &&
	git add init.t &&
	test_must_fail git commit --no-verify -m "local override" 2>err &&
	test_grep "hook.allowNoVerify" err
'

test_expect_success 'CLI -c hook.allowNoVerify=true overrides local false' '
	test_config hook.allowNoVerify false &&
	echo "change10" >>init.t &&
	git add init.t &&
	git -c hook.allowNoVerify=true commit --no-verify -m "override false with CLI true"
'

test_expect_success 'hook.allowNoVerify=false provides emergency override advice' '
	test_config hook.allowNoVerify false &&
	echo "change11" >>init.t &&
	git add init.t &&
	test_must_fail git commit --no-verify -m "fail advice" 2>err &&
	test_grep "GIT_ALLOW_NO_VERIFY=1" err &&
	test_grep "git -c hook.allowNoVerify=true" err
'

test_expect_success 'GIT_ALLOW_NO_VERIFY=1 permits git commit --no-verify even when configured to false' '
	test_when_finished "rm -f pre-commit.log" &&
	test_config hook.allowNoVerify false &&
	echo "change12" >>init.t &&
	git add init.t &&
	GIT_ALLOW_NO_VERIFY=1 git commit --no-verify -m "emergency commit" &&
	test_path_is_missing pre-commit.log
'

test_expect_success 'GIT_ALLOW_NO_VERIFY=1 permits git push --no-verify even when configured to false' '
	test_when_finished "rm -f pre-push.log" &&
	test_config hook.allowNoVerify false &&
	GIT_ALLOW_NO_VERIFY=1 git push --no-verify origin main &&
	test_path_is_missing pre-push.log
'

test_expect_success 'hook.allowNoVerify=warn permits --no-verify and warns on stderr' '
	test_when_finished "rm -f pre-commit.log err" &&
	test_config hook.allowNoVerify warn &&
	echo "change13" >>init.t &&
	git add init.t &&
	git commit --no-verify -m "commit with warn" 2>err &&
	test_path_is_missing pre-commit.log &&
	test_grep "bypassing hooks with .--no-verify. is discouraged" err
'

test_expect_success 'hook.allowNoVerify=0 disallows --no-verify' '
	test_config hook.allowNoVerify 0 &&
	echo "change14" >>init.t &&
	git add init.t &&
	test_must_fail git commit --no-verify -m "fail 0" 2>err &&
	test_grep "hook.allowNoVerify" err
'

test_expect_success 'hook.allowNoVerify=1 allows --no-verify' '
	test_when_finished "rm -f pre-commit.log" &&
	test_config hook.allowNoVerify 1 &&
	echo "change15" >>init.t &&
	git add init.t &&
	git commit --no-verify -m "commit 1" &&
	test_path_is_missing pre-commit.log
'

test_expect_success 'legacy hooks.allowNoVerify (plural) is accepted as fallback' '
	test_config hooks.allowNoVerify false &&
	echo "change16" >>init.t &&
	git add init.t &&
	test_must_fail git commit --no-verify -m "fail fallback" 2>err &&
	test_grep "hook.allowNoVerify" err
'

test_done
