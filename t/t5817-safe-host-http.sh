#!/bin/sh

test_description='safe.host restricts the hosts Git connects to over HTTP'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-httpd.sh
start_httpd

# Without CURLOPT_PREREQFUNCTION, Git cannot check where a redirect leads
# before following it, and so does not follow any.
test_lazy_prereq CURL_PREREQFUNCTION '
	curl_version=$(git version --build-options | sed -n "s/^libcurl: //p") &&
	test -n "$curl_version" &&
	lowest=$(printf "%s\n" 7.80.0 "$curl_version" |
		 sort -t. -k1,1n -k2,2n -k3,3n | head -n 1) &&
	test "$lowest" = 7.80.0
'

test_expect_success 'setup' '
	test_commit one &&
	git init --bare "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" &&
	git push "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" HEAD
'

test_expect_success 'listed host is allowed' '
	git -c safe.host=127.0.0.1 ls-remote "$HTTPD_URL/smart/repo.git" >out &&
	test_grep refs/heads/ out
'

test_expect_success 'unlisted host is denied before connecting' '
	test_must_fail git -c safe.host=example.com \
		ls-remote "$HTTPD_URL/smart/never-requested.git" 2>err &&
	test_grep "connecting to .$HTTPD_URL. is not allowed by safe.host" err &&
	test_grep "git config --global --add safe.host $HTTPD_URL$" err &&
	test_grep ! never-requested "$HTTPD_ROOT_PATH/access.log"
'

test_expect_success 'scheme and port are checked' '
	git -c safe.host="$HTTPD_PROTO://127.0.0.1:$LIB_HTTPD_PORT" \
		ls-remote "$HTTPD_URL/smart/repo.git" &&
	test_must_fail git -c safe.host=127.0.0.1:1 \
		ls-remote "$HTTPD_URL/smart/repo.git" 2>err &&
	test_grep "connecting to .$HTTPD_URL. is not allowed by safe.host" err &&
	test_must_fail git -c safe.host=ssh://127.0.0.1 \
		ls-remote "$HTTPD_URL/smart/repo.git" 2>err &&
	test_grep "connecting to .$HTTPD_URL. is not allowed by safe.host" err
'

test_expect_success 'redirect to another host is followed by default' '
	git ls-remote "$HTTPD_URL/smart-redir-localhost/repo.git" >out &&
	test_grep refs/heads/ out
'

test_expect_success CURL_PREREQFUNCTION 'redirect to listed host is followed' '
	git -c safe.host=127.0.0.1 -c safe.host=localhost \
		ls-remote "$HTTPD_URL/smart-redir-localhost/repo.git" >out &&
	test_grep refs/heads/ out
'

test_expect_success 'redirect to unlisted host is denied' '
	test_must_fail git -c safe.host=127.0.0.1 \
		ls-remote "$HTTPD_URL/smart-redir-localhost/repo.git" 2>err &&
	if test_have_prereq CURL_PREREQFUNCTION
	then
		test_grep "connecting to .$HTTPD_PROTO://localhost:$LIB_HTTPD_PORT. is not allowed by safe.host" err
	fi
'

test_expect_success !CURL_PREREQFUNCTION 'redirects are not followed' '
	test_must_fail git -c safe.host=127.0.0.1 \
		ls-remote "$HTTPD_URL/smart-redir-perm/repo.git"
'

test_expect_success CURL_PREREQFUNCTION 'http-alternates on unlisted hosts are ignored' '
	victim=$HTTPD_DOCUMENT_ROOT_PATH/victim.git &&
	git init --bare "$victim" &&
	git -C "$victim" --work-tree=. commit --allow-empty -m secret &&
	git -C "$victim" repack -ad &&
	git -C "$victim" update-server-info &&
	oid=$(git -C "$victim" rev-parse HEAD) &&

	evil=$HTTPD_DOCUMENT_ROOT_PATH/evil.git &&
	git init --template= --bare "$evil" &&
	mkdir "$evil/info" &&
	# do this by hand to avoid object existence check
	printf "%s\\t%s\\n" $oid refs/heads/main >"$evil/info/refs" &&
	echo "$HTTPD_PROTO://localhost:$LIB_HTTPD_PORT/dumb/victim.git/objects" \
		>"$evil/objects/info/http-alternates" &&

	test_must_fail git -c http.followRedirects=true -c safe.host=127.0.0.1 \
		clone "$HTTPD_URL/dumb/evil.git" evil-denied 2>err &&
	test_grep "ignoring alternate on host not allowed by safe.host" err &&
	git -c http.followRedirects=true \
		-c safe.host=127.0.0.1 -c safe.host=localhost \
		clone "$HTTPD_URL/dumb/evil.git" evil-allowed
'

test_done
