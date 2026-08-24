#!/bin/sh

test_description='basic sanity checks for git whoami'

. ./test-lib.sh
. "$TEST_DIRECTORY/lib-gpg.sh"

test_expect_success 'default output format without signing' '
	cat >expect <<-EOF &&
	Author:    $GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>
	Committer: $GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>
	Signing:   disabled (commit.gpgsign: false)
	EOF
	git whoami >actual &&
	test_cmp expect actual
'

test_expect_success 'git whoami --author' '
	echo "$GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>" >expect &&
	git whoami --author >actual &&
	test_cmp expect actual
'

test_expect_success 'git whoami --committer' '
	echo "$GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>" >expect &&
	git whoami --committer >actual &&
	test_cmp expect actual
'

test_expect_success 'git whoami --author --name and --author --email' '
	echo "$GIT_AUTHOR_NAME" >expect_name &&
	git whoami --author --name >actual_name &&
	test_cmp expect_name actual_name &&
	echo "$GIT_AUTHOR_EMAIL" >expect_email &&
	git whoami --author --email >actual_email &&
	test_cmp expect_email actual_email
'

test_expect_success 'git whoami --committer --name and --committer --email' '
	echo "$GIT_COMMITTER_NAME" >expect_name &&
	git whoami --committer --name >actual_name &&
	test_cmp expect_name actual_name &&
	echo "$GIT_COMMITTER_EMAIL" >expect_email &&
	git whoami --committer --email >actual_email &&
	test_cmp expect_email actual_email
'

test_expect_success 'git whoami --signing-key when signing is disabled and unset' '
	test_config commit.gpgsign false &&
	test_unconfig user.signingkey &&
	test_must_fail git whoami --signing-key
'

test_expect_success 'git whoami with explicitly configured signing key' '
	test_config user.signingkey "TEST_KEY_123" &&
	test_config commit.gpgsign true &&
	test_config gpg.format ssh &&
	cat >expect <<-EOF &&
	Author:    $GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>
	Committer: $GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>
	Signing:   TEST_KEY_123 (format: ssh, commit.gpgsign: true)
	EOF
	git whoami >actual &&
	test_cmp expect actual &&
	echo "TEST_KEY_123" >expect_key &&
	git whoami --signing-key >actual_key &&
	test_cmp expect_key actual_key
'

test_expect_success 'git whoami with signing disabled but key configured' '
	test_config user.signingkey "TEST_KEY_123" &&
	test_config commit.gpgsign false &&
	test_config gpg.format openpgp &&
	cat >expect <<-EOF &&
	Author:    $GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>
	Committer: $GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>
	Signing:   disabled (key: TEST_KEY_123, format: openpgp, commit.gpgsign: false)
	EOF
	git whoami >actual &&
	test_cmp expect actual &&
	echo "TEST_KEY_123" >expect_key &&
	git whoami --signing-key >actual_key &&
	test_cmp expect_key actual_key
'

test_expect_success 'git whoami with openpgp signing enabled without explicit key' '
	test_unconfig user.signingkey &&
	test_config commit.gpgsign true &&
	test_config gpg.format openpgp &&
	cat >expect <<-EOF &&
	Author:    $GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>
	Committer: $GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>
	Signing:   default key ($GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>) (format: openpgp, commit.gpgsign: true)
	EOF
	git whoami >actual &&
	test_cmp expect actual &&
	echo "$GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>" >expect_key &&
	git whoami --signing-key >actual_key &&
	test_cmp expect_key actual_key
'

test_expect_success 'git whoami with ssh signing enabled without explicit key' '
	test_unconfig user.signingkey &&
	test_config commit.gpgsign true &&
	test_config gpg.format ssh &&
	test_unconfig gpg.ssh.defaultKeyCommand &&
	cat >expect <<-EOF &&
	Author:    $GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>
	Committer: $GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>
	Signing:   enabled (no signing key configured)
	EOF
	git whoami >actual &&
	test_cmp expect actual &&
	test_must_fail git whoami --signing-key
'

test_expect_success GPGSSH 'git whoami with ssh defaultKeyCommand' '
	test_unconfig user.signingkey &&
	test_config commit.gpgsign true &&
	test_config gpg.format ssh &&
	test_config gpg.ssh.defaultKeyCommand "cat \"${GPGSSH_KEY_PRIMARY}.pub\"" &&
	git whoami --signing-key >actual_key &&
	test_grep "^SHA256:" actual_key
'

test_expect_success 'git whoami -v / --verbose' '
	test_config user.signingkey "MY_SIGNING_KEY" &&
	test_config commit.gpgsign true &&
	test_config gpg.format openpgp &&
	cat >expect <<-EOF &&
	Author Name:      $GIT_AUTHOR_NAME
	Author Email:     $GIT_AUTHOR_EMAIL
	Committer Name:   $GIT_COMMITTER_NAME
	Committer Email:  $GIT_COMMITTER_EMAIL
	Signing Key:      MY_SIGNING_KEY
	Signing Format:   openpgp
	GPG Signing:      enabled
	EOF
	git whoami -v >actual &&
	test_cmp expect actual
'

test_expect_success 'git whoami with environment variable overrides' '
	test_unconfig user.signingkey &&
	test_config commit.gpgsign false &&
	cat >expect <<-EOF &&
	Author:    Custom Author <custom.author@example.com>
	Committer: Custom Committer <custom.committer@example.com>
	Signing:   disabled (commit.gpgsign: false)
	EOF
	GIT_AUTHOR_NAME="Custom Author" \
	GIT_AUTHOR_EMAIL="custom.author@example.com" \
	GIT_COMMITTER_NAME="Custom Committer" \
	GIT_COMMITTER_EMAIL="custom.committer@example.com" \
	git whoami >actual &&
	test_cmp expect actual
'

test_expect_success 'incompatible option combinations fail' '
	test_must_fail git whoami --author --committer 2>err &&
	test_grep "cannot be used together" err &&
	test_must_fail git whoami --name --email 2>err &&
	test_grep "cannot be used together" err &&
	test_must_fail git whoami --signing-key --name 2>err &&
	test_grep "cannot be used together" err &&
	test_must_fail git whoami --signing-key --email 2>err &&
	test_grep "cannot be used together" err &&
	test_must_fail git whoami --signing-key --author 2>err &&
	test_grep "cannot be used together" err &&
	test_must_fail git whoami --signing-key --committer 2>err &&
	test_grep "cannot be used together" err &&
	test_must_fail git whoami --verbose --name 2>err &&
	test_grep "cannot be used together" err
'

test_expect_success 'git whoami outside of repository' '
	nongit git whoami --author >actual &&
	test_grep "<" actual
'

test_done
