test_expect_success "add two receive.procReceiveRefs settings" '
	(
		cd "$upstream" && GIT_DIR=. && export GIT_DIR &&
		git config --add receive.procReceiveRefs refs/for &&
		git config --add receive.procReceiveRefs refs/review/
	)
'
