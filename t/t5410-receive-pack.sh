#!/bin/sh

test_description='git receive-pack'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

test_expect_success 'setup' '
	test_commit base &&
	git clone -s --bare . fork &&
	git checkout -b public/branch main &&
	test_commit public &&
	git checkout -b private/branch main &&
	test_commit private
'

extract_haves () {
	depacketize | sed -n 's/^\([^ ][^ ]*\) \.have/\1/p'
}

test_expect_success 'with core.alternateRefsCommand' '
	write_script fork/alternate-refs <<-\EOF &&
		git --git-dir="$1" for-each-ref \
			--format="%(objectname)" \
			refs/heads/public/
	EOF
	test_config -C fork core.alternateRefsCommand ./alternate-refs &&
	git rev-parse public/branch >expect &&
	printf "0000" | git receive-pack fork >actual &&
	extract_haves <actual >actual.haves &&
	test_cmp expect actual.haves
'

test_expect_success 'with core.alternateRefsPrefixes' '
	test_config -C fork core.alternateRefsPrefixes "refs/heads/private" &&
	git rev-parse private/branch >expect &&
	printf "0000" | git receive-pack fork >actual &&
	extract_haves <actual >actual.haves &&
	test_cmp expect actual.haves
'

# The `tee.exe` shipped in Git for Windows v2.49.0 is known to hang frequently
# when spawned from `git.exe` and piping its output to `git.exe`. This seems
# related to MSYS2 runtime bug fixes regarding the signal handling; Let's just
# skip the tests that need to exercise this when the faulty MSYS2 runtime is
# detected; The test cases are exercised enough in other matrix jobs of the CI
# runs.
test_lazy_prereq TEE_DOES_NOT_HANG '
	test_have_prereq !MINGW &&
	case "$(uname -a)" in *3.5.7-463ebcdc.x86_64*) false;; esac
'

test_expect_success TEE_DOES_NOT_HANG \
	'receive-pack missing objects fails connectivity check' '
	test_when_finished rm -rf repo remote.git setup.git &&

	git init repo &&
	git -C repo commit --allow-empty -m 1 &&
	git clone --bare repo setup.git &&
	git -C repo commit --allow-empty -m 2 &&

	# Capture git-send-pack(1) output sent to git-receive-pack(1).
	git -C repo send-pack ../setup.git --all \
		--receive-pack="tee ${SQ}$(pwd)/out${SQ} | git-receive-pack" &&

	# Replay captured git-send-pack(1) output on new empty repository.
	git init --bare remote.git &&
	git receive-pack remote.git <out >actual 2>err &&

	test_grep "missing necessary objects" actual &&
	test_grep "fatal: Failed to traverse parents" err &&
	test_must_fail git -C remote.git cat-file -e $(git -C repo rev-parse HEAD)
'

test_expect_success TEE_DOES_NOT_HANG \
	'receive-pack missing objects bypasses connectivity check' '
	test_when_finished rm -rf repo remote.git setup.git &&

	git init repo &&
	git -C repo commit --allow-empty -m 1 &&
	git clone --bare repo setup.git &&
	git -C repo commit --allow-empty -m 2 &&

	# Capture git-send-pack(1) output sent to git-receive-pack(1).
	git -C repo send-pack ../setup.git --all \
		--receive-pack="tee ${SQ}$(pwd)/out${SQ} | git-receive-pack" &&

	# Replay captured git-send-pack(1) output on new empty repository.
	git init --bare remote.git &&
	git receive-pack --skip-connectivity-check remote.git <out >actual 2>err &&

	test_grep ! "missing necessary objects" actual &&
	test_must_be_empty err &&
	git -C remote.git cat-file -e $(git -C repo rev-parse HEAD) &&
	test_must_fail git -C remote.git rev-list $(git -C repo rev-parse HEAD)
'

test_expect_success 'connectivity check uses narrow stopper, omits --alternate-refs' '
	test_when_finished "rm -rf src dst trace" &&

	git init src &&
	test_commit -C src one &&

	# Clone first so that HEAD in the bare repo resolves; this lets us
	# observe HEAD in the stopper args.
	git clone --bare src dst &&
	test_commit -C src two &&

	GIT_TRACE="$(pwd)/trace" \
		git -C src push ../dst HEAD:refs/heads/main &&

	# Receive-pack should request the narrow stopper.  GIT_TRACE may
	# single-quote arguments containing shell metacharacters, so allow
	# an optional quote before/after --exclude=*/*.
	grep "rev-list .*--not .\{0,1\}--exclude=\*/\*.\{0,1\} --branches HEAD" \
		trace &&
	# And not fall back to --alternate-refs in this code path.
	! grep "rev-list .*--alternate-refs" trace
'

test_expect_success 'push to bare repo with unborn HEAD succeeds' '
	test_when_finished "rm -rf src dst" &&

	git init src &&
	test_commit -C src one &&

	# Fresh bare repo: HEAD points at an unborn branch and no
	# branches exist yet, so the narrow stopper code must omit HEAD
	# and "--branches" expands to nothing.
	git init --bare dst &&
	git -C src push ../dst HEAD:refs/heads/main &&
	git -C src rev-parse HEAD >expect &&
	git -C dst rev-parse refs/heads/main >actual &&
	test_cmp expect actual
'

test_expect_success 'push to bare repo whose HEAD resolves to a nested branch succeeds' '
	test_when_finished "rm -rf src dst" &&

	git init src &&
	test_commit -C src one &&

	git init --bare dst &&
	git -C src push ../dst HEAD:refs/heads/team/feature &&
	# Point HEAD at the nested branch so HEAD resolves, but the
	# only branch in the repo is excluded by "--exclude=*/*".
	git -C dst symbolic-ref HEAD refs/heads/team/feature &&

	test_commit -C src two &&
	git -C src push ../dst HEAD:refs/heads/team/feature &&
	git -C src rev-parse HEAD >expect &&
	git -C dst rev-parse refs/heads/team/feature >actual &&
	test_cmp expect actual
'

test_done
