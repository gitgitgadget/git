#!/bin/sh

test_description='basic sanity checks for git ident'

. ./test-lib.sh
. "$TEST_DIRECTORY/lib-gpg.sh"

test_expect_success 'default output format (author + committer)' '
	cat >expect <<-EOF &&
	$GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>
	$GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>
	EOF
	git ident >actual &&
	test_cmp expect actual
'

test_expect_success 'git ident -v / --verbose default' '
	cat >expect <<-EOF &&
	Author: $GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>
	Committer: $GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>
	EOF
	git ident -v >actual &&
	test_cmp expect actual
'

test_expect_success 'git ident --author / -a' '
	echo "$GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>" >expect &&
	git ident --author >actual &&
	test_cmp expect actual &&
	git ident -a >actual_short &&
	test_cmp expect actual_short
'

test_expect_success 'git ident --committer / -c' '
	echo "$GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>" >expect &&
	git ident --committer >actual &&
	test_cmp expect actual &&
	git ident -c >actual_short &&
	test_cmp expect actual_short
'

test_expect_success 'git ident -a -c' '
	cat >expect <<-EOF &&
	$GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>
	$GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>
	EOF
	git ident -a -c >actual &&
	test_cmp expect actual
'

test_expect_success 'git ident -a -n and -a -e' '
	echo "$GIT_AUTHOR_NAME" >expect_name &&
	git ident -a -n >actual_name &&
	test_cmp expect_name actual_name &&
	echo "<$GIT_AUTHOR_EMAIL>" >expect_email &&
	git ident -a -e >actual_email &&
	test_cmp expect_email actual_email
'

test_expect_success 'git ident -c -n and -c -e' '
	echo "$GIT_COMMITTER_NAME" >expect_name &&
	git ident -c -n >actual_name &&
	test_cmp expect_name actual_name &&
	echo "<$GIT_COMMITTER_EMAIL>" >expect_email &&
	git ident -c -e >actual_email &&
	test_cmp expect_email actual_email
'

test_expect_success 'git ident -a -e -n (additive full ident)' '
	echo "$GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>" >expect &&
	git ident -a -e -n >actual &&
	test_cmp expect actual
'

test_expect_success 'git ident -a -n -v (labeled name)' '
	echo "Author: $GIT_AUTHOR_NAME" >expect &&
	git ident -a -n -v >actual &&
	test_cmp expect actual
'

test_expect_success 'git ident -a -e -v (labeled email)' '
	echo "Author: <$GIT_AUTHOR_EMAIL>" >expect &&
	git ident -a -e -v >actual &&
	test_cmp expect actual
'

test_expect_success 'git ident -a -c -e (unlabeled emails)' '
	cat >expect <<-EOF &&
	<$GIT_AUTHOR_EMAIL>
	<$GIT_COMMITTER_EMAIL>
	EOF
	git ident -a -c -e >actual &&
	test_cmp expect actual
'

test_expect_success 'git ident -a -c -e -v (labeled emails)' '
	cat >expect <<-EOF &&
	Author: <$GIT_AUTHOR_EMAIL>
	Committer: <$GIT_COMMITTER_EMAIL>
	EOF
	git ident -a -c -e -v >actual &&
	test_cmp expect actual
'

test_expect_success 'git ident -a -c -n -v (labeled names)' '
	cat >expect <<-EOF &&
	Author: $GIT_AUTHOR_NAME
	Committer: $GIT_COMMITTER_NAME
	EOF
	git ident -a -c -n -v >actual &&
	test_cmp expect actual
'

test_expect_success 'git ident with -z on single fields' '
	printf "$GIT_AUTHOR_NAME\0" >expect_name &&
	git ident -a -n -z >actual_name &&
	test_cmp expect_name actual_name &&
	printf "<$GIT_AUTHOR_EMAIL>\0" >expect_email &&
	git ident -a -e -z >actual_email &&
	test_cmp expect_email actual_email
'

test_expect_success 'git ident with -z on multiple entries' '
	printf "$GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>\0$GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>\0" >expect &&
	git ident -z >actual &&
	test_cmp expect actual
'

test_expect_success 'git ident --porcelain format' '
	test_config user.signingkey "TEST_KEY_123" &&
	test_config commit.gpgsign true &&
	test_config gpg.format ssh &&
	cat >expect <<-EOF &&
	user.author.name=$GIT_AUTHOR_NAME
	user.author.email=$GIT_AUTHOR_EMAIL
	user.committer.name=$GIT_COMMITTER_NAME
	user.committer.email=$GIT_COMMITTER_EMAIL
	user.signingkey=TEST_KEY_123
	gpg.format=ssh
	commit.gpgsign=true
	EOF
	git ident --porcelain >actual &&
	test_cmp expect actual
'

test_expect_success 'git ident -a --porcelain format' '
	cat >expect <<-EOF &&
	user.author.name=$GIT_AUTHOR_NAME
	user.author.email=$GIT_AUTHOR_EMAIL
	EOF
	git ident -a --porcelain >actual &&
	test_cmp expect actual
'

test_expect_success 'git ident --porcelain -z format' '
	test_config user.signingkey "TEST_KEY_123" &&
	test_config commit.gpgsign true &&
	test_config gpg.format ssh &&
	printf "user.author.name=$GIT_AUTHOR_NAME\0user.author.email=$GIT_AUTHOR_EMAIL\0user.committer.name=$GIT_COMMITTER_NAME\0user.committer.email=$GIT_COMMITTER_EMAIL\0user.signingkey=TEST_KEY_123\0gpg.format=ssh\0commit.gpgsign=true\0" >expect &&
	git ident --porcelain -z >actual &&
	test_cmp expect actual
'

test_expect_success 'git ident --signing-key when signing is disabled and unset' '
	test_config commit.gpgsign false &&
	test_unconfig user.signingkey &&
	test_must_fail git ident --signing-key
'

test_expect_success 'git ident with explicitly configured signing key' '
	test_config user.signingkey "TEST_KEY_123" &&
	test_config commit.gpgsign true &&
	test_config gpg.format ssh &&
	echo "TEST_KEY_123" >expect_key &&
	git ident --signing-key >actual_key &&
	test_cmp expect_key actual_key
'

test_expect_success 'git ident with openpgp signing enabled without explicit key' '
	test_unconfig user.signingkey &&
	test_config commit.gpgsign true &&
	test_config gpg.format openpgp &&
	echo "$GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>" >expect_key &&
	git ident --signing-key >actual_key &&
	test_cmp expect_key actual_key
'

test_expect_success 'git ident with ssh signing enabled without explicit key' '
	test_unconfig user.signingkey &&
	test_config commit.gpgsign true &&
	test_config gpg.format ssh &&
	test_unconfig gpg.ssh.defaultKeyCommand &&
	test_must_fail git ident --signing-key
'

test_expect_success GPGSSH 'git ident with ssh defaultKeyCommand' '
	test_unconfig user.signingkey &&
	test_config commit.gpgsign true &&
	test_config gpg.format ssh &&
	test_config gpg.ssh.defaultKeyCommand "cat \"${GPGSSH_KEY_PRIMARY}.pub\"" &&
	git ident --signing-key >actual_key &&
	test_grep "^SHA256:" actual_key
'

test_expect_success 'git ident with environment variable overrides' '
	test_unconfig user.signingkey &&
	test_config commit.gpgsign false &&
	cat >expect <<-EOF &&
	Custom Author <custom.author@example.com>
	Custom Committer <custom.committer@example.com>
	EOF
	GIT_AUTHOR_NAME="Custom Author" \
	GIT_AUTHOR_EMAIL="custom.author@example.com" \
	GIT_COMMITTER_NAME="Custom Committer" \
	GIT_COMMITTER_EMAIL="custom.committer@example.com" \
	git ident >actual &&
	test_cmp expect actual
'

test_expect_success 'incompatible option combinations fail' '
	test_must_fail git ident --porcelain --verbose 2>err &&
	test_grep "cannot be used together" err &&
	test_must_fail git ident --porcelain --name 2>err &&
	test_grep "cannot be used together" err &&
	test_must_fail git ident --porcelain --email 2>err &&
	test_grep "cannot be used together" err
'

test_expect_success 'git ident outside of repository' '
	nongit git ident --author >actual &&
	test_grep "<" actual
'

test_done
