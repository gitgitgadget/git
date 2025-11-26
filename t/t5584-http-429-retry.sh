#!/bin/sh

test_description='test HTTP 429 Too Many Requests retry logic'

. ./test-lib.sh

. "$TEST_DIRECTORY"/lib-httpd.sh

start_httpd

test_expect_success 'setup test repository' '
	test_commit initial &&
	git clone --bare . "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" &&
	git --git-dir="$HTTPD_DOCUMENT_ROOT_PATH/repo.git" config http.receivepack true
'

# This test suite uses a special HTTP 429 endpoint at /http_429/ that simulates
# rate limiting. The endpoint format is:
#   /http_429/<test-context>/<retry-after-value>/<repo-path>
# The http-429.sh script (in t/lib-httpd) returns a 429 response with the
# specified Retry-After header on the first request for each test context,
# then forwards subsequent requests to git-http-backend. Each test context
# is isolated, allowing multiple tests to run independently.

test_expect_success 'HTTP 429 with retries disabled (maxRetries=0) fails immediately' '
	# Set maxRetries to 0 (disabled)
	test_config http.maxRetries 0 &&
	test_config http.retryAfter 1 &&

	# Should fail immediately without any retry attempt
	test_must_fail git ls-remote "$HTTPD_URL/http_429/retries-disabled/1/repo.git" 2>err &&

	# Verify no retry happened (no "waiting" message in stderr)
	test_grep ! -i "waiting.*retry" err
'

test_expect_success 'HTTP 429 permanent should fail after max retries' '
	# Enable retries with a limit
	test_config http.maxRetries 2 &&

	# Git should retry but eventually fail when 429 persists
	test_must_fail git ls-remote "$HTTPD_URL/http_429/permanent-fail/permanent/repo.git" 2>err
'

test_expect_success 'HTTP 429 with Retry-After is retried and succeeds' '
	# Enable retries
	test_config http.maxRetries 3 &&

	# Git should retry after receiving 429 and eventually succeed
	git ls-remote "$HTTPD_URL/http_429/retry-succeeds/1/repo.git" >output 2>err &&
	test_grep "refs/heads/" output
'

test_expect_success 'HTTP 429 without Retry-After uses configured default' '
	# Enable retries and configure default delay
	test_config http.maxRetries 3 &&
	test_config http.retryAfter 1 &&

	# Git should retry using configured default and succeed
	git ls-remote "$HTTPD_URL/http_429/no-retry-after-header/none/repo.git" >output 2>err &&
	test_grep "refs/heads/" output
'

test_expect_success 'HTTP 429 retry delays are respected' '
	# Enable retries
	test_config http.maxRetries 3 &&

	# Time the operation - it should take at least 2 seconds due to retry delay
	start=$(date +%s) &&
	git ls-remote "$HTTPD_URL/http_429/retry-delays-respected/2/repo.git" >output 2>err &&
	end=$(date +%s) &&
	duration=$((end - start)) &&

	# Verify it took at least 2 seconds (allowing some tolerance)
	test "$duration" -ge 1 &&
	test_grep "refs/heads/" output
'

test_expect_success 'HTTP 429 fails immediately if Retry-After exceeds http.maxRetryTime' '
	# Configure max retry time to 3 seconds (much less than requested 100)
	test_config http.maxRetries 3 &&
	test_config http.maxRetryTime 3 &&

	# Should fail immediately without waiting
	start=$(date +%s) &&
	test_must_fail git ls-remote "$HTTPD_URL/http_429/retry-after-exceeds-max-time/100/repo.git" 2>err &&
	end=$(date +%s) &&
	duration=$((end - start)) &&

	# Should fail quickly (less than 2 seconds, no 100 second wait)
	test "$duration" -lt 2 &&
	test_grep "exceeds http.maxRetryTime" err
'

test_expect_success 'HTTP 429 fails if configured http.retryAfter exceeds http.maxRetryTime' '
	# Test misconfiguration: retryAfter > maxRetryTime
	# Configure retryAfter larger than maxRetryTime
	test_config http.maxRetries 3 &&
	test_config http.retryAfter 100 &&
	test_config http.maxRetryTime 5 &&

	# Should fail immediately with configuration error
	start=$(date +%s) &&
	test_must_fail git ls-remote "$HTTPD_URL/http_429/config-retry-after-exceeds-max-time/none/repo.git" 2>err &&
	end=$(date +%s) &&
	duration=$((end - start)) &&

	# Should fail quickly
	test "$duration" -lt 2 &&
	test_grep "configured http.retryAfter.*exceeds.*http.maxRetryTime" err
'

test_expect_success 'HTTP 429 with Retry-After HTTP-date format' '
	# Test HTTP-date format (RFC 2822) in Retry-After header
	# Generate a date 2 seconds in the future
	future_date=$(TZ=GMT date -d "+2 seconds" "+%a, %d %b %Y %H:%M:%S GMT" 2>/dev/null || \
		      TZ=GMT date -v+2S "+%a, %d %b %Y %H:%M:%S GMT" 2>/dev/null || \
		      echo "skip") &&

	if test "$future_date" = "skip"
	then
		skip_all="date command does not support required format" &&
		test_done
	fi &&

	# URL-encode the date (replace spaces with %20)
	future_date_encoded=$(echo "$future_date" | sed "s/ /%20/g") &&

	# Enable retries
	test_config http.maxRetries 3 &&

	# Git should parse the HTTP-date and retry after the delay
	start=$(date +%s) &&
	git ls-remote "$HTTPD_URL/http_429/http-date-format/$future_date_encoded/repo.git" >output 2>err &&
	end=$(date +%s) &&
	duration=$((end - start)) &&

	# Should take at least 1 second (allowing tolerance for processing time)
	test "$duration" -ge 1 &&
	test_grep "refs/heads/" output
'

test_expect_success 'HTTP 429 with HTTP-date exceeding maxRetryTime fails immediately' '
	# Generate a date 200 seconds in the future
	future_date=$(TZ=GMT date -d "+200 seconds" "+%a, %d %b %Y %H:%M:%S GMT" 2>/dev/null || \
		      TZ=GMT date -v+200S "+%a, %d %b %Y %H:%M:%S GMT" 2>/dev/null || \
		      echo "skip") &&

	if test "$future_date" = "skip"
	then
		skip_all="date command does not support required format" &&
		test_done
	fi &&

	# URL-encode the date (replace spaces with %20)
	future_date_encoded=$(echo "$future_date" | sed "s/ /%20/g") &&

	# Configure max retry time much less than the 200 second delay
	test_config http.maxRetries 3 &&
	test_config http.maxRetryTime 10 &&

	# Should fail immediately without waiting 200 seconds
	start=$(date +%s) &&
	test_must_fail git ls-remote "$HTTPD_URL/http_429/http-date-exceeds-max-time/$future_date_encoded/repo.git" 2>err &&
	end=$(date +%s) &&
	duration=$((end - start)) &&

	# Should fail quickly (not wait 200 seconds)
	test "$duration" -lt 2 &&
	test_grep "exceeds http.maxRetryTime" err
'

test_expect_success 'HTTP 429 with past HTTP-date should not wait' '
	past_date=$(TZ=GMT date -d "-10 seconds" "+%a, %d %b %Y %H:%M:%S GMT" 2>/dev/null || \
		    TZ=GMT date -v-10S "+%a, %d %b %Y %H:%M:%S GMT" 2>/dev/null || \
		    echo "skip") &&

	if test "$past_date" = "skip"
	then
		skip_all="date command does not support required format" &&
		test_done
	fi &&

	# URL-encode the date (replace spaces with %20)
	past_date_encoded=$(echo "$past_date" | sed "s/ /%20/g") &&

	# Enable retries
	test_config http.maxRetries 3 &&

	# Git should retry immediately without waiting
	start=$(date +%s) &&
	git ls-remote "$HTTPD_URL/http_429/past-http-date/$past_date_encoded/repo.git" >output 2>err &&
	end=$(date +%s) &&
	duration=$((end - start)) &&

	# Should complete quickly (less than 2 seconds)
	test "$duration" -lt 2 &&
	test_grep "refs/heads/" output
'

test_expect_success 'HTTP 429 with invalid Retry-After format uses configured default' '
	# Configure default retry-after
	test_config http.maxRetries 3 &&
	test_config http.retryAfter 1 &&

	# Should use configured default (1 second) since header is invalid
	start=$(date +%s) &&
	git ls-remote "$HTTPD_URL/http_429/invalid-retry-after-format/invalid/repo.git" >output 2>err &&
	end=$(date +%s) &&
	duration=$((end - start)) &&

	# Should take at least 1 second (the configured default)
	test "$duration" -ge 1 &&
	test_grep "refs/heads/" output &&
	test_grep "waiting.*retry" err
'

test_expect_success 'HTTP 429 will not be retried without config' '
	# Default config means http.maxRetries=0 (retries disabled)
	# When 429 is received, it should fail immediately without retry
	# Do NOT configure anything - use defaults (http.maxRetries defaults to 0)

	# Should fail immediately without retry
	test_must_fail git ls-remote "$HTTPD_URL/http_429/no-retry-without-config/1/repo.git" 2>err &&

	# Verify no retry happened (no "waiting" message)
	test_grep ! -i "waiting.*retry" err &&

	# Should get 429 error
	test_grep "429" err
'

test_expect_success 'GIT_HTTP_RETRY_AFTER overrides http.retryAfter config' '
	# Configure retryAfter to 10 seconds
	test_config http.maxRetries 3 &&
	test_config http.retryAfter 10 &&

	# Override with environment variable to 1 second
	start=$(date +%s) &&
	GIT_HTTP_RETRY_AFTER=1 git ls-remote "$HTTPD_URL/http_429/env-retry-after-override/none/repo.git" >output 2>err &&
	end=$(date +%s) &&
	duration=$((end - start)) &&

	# Should use env var (1 second), not config (10 seconds)
	test "$duration" -ge 1 &&
	test "$duration" -lt 5 &&
	test_grep "refs/heads/" output &&
	test_grep "waiting.*retry" err
'

test_expect_success 'GIT_HTTP_MAX_RETRIES overrides http.maxRetries config' '
	# Configure maxRetries to 0 (disabled)
	test_config http.maxRetries 0 &&
	test_config http.retryAfter 1 &&

	# Override with environment variable to enable retries
	GIT_HTTP_MAX_RETRIES=3 git ls-remote "$HTTPD_URL/http_429/env-max-retries-override/1/repo.git" >output 2>err &&

	# Should retry (env var enables it despite config saying disabled)
	test_grep "refs/heads/" output &&
	test_grep "waiting.*retry" err
'

test_expect_success 'GIT_HTTP_MAX_RETRY_TIME overrides http.maxRetryTime config' '
	# Configure maxRetryTime to 100 seconds (would accept 50 second delay)
	test_config http.maxRetries 3 &&
	test_config http.maxRetryTime 100 &&

	# Override with environment variable to 10 seconds (should reject 50 second delay)
	start=$(date +%s) &&
	test_must_fail env GIT_HTTP_MAX_RETRY_TIME=10 \
		git ls-remote "$HTTPD_URL/http_429/env-max-retry-time-override/50/repo.git" 2>err &&
	end=$(date +%s) &&
	duration=$((end - start)) &&

	# Should fail quickly (not wait 50 seconds) because env var limits to 10
	test "$duration" -lt 5 &&
	test_grep "exceeds http.maxRetryTime" err
'

test_expect_success 'verify normal repository access still works' '
	git ls-remote "$HTTPD_URL/smart/repo.git" >output &&
	test_grep "refs/heads/" output
'

test_done
