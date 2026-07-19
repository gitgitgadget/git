#!/bin/sh

if test $# = 3
then
	merge=$1 base=$2 target=$3
	label=$(git log --oneline -1 "$merge")

	base0=$(git rev-parse "$base^0") &&
	base1=$(git rev-parse "$merge^2") &&
	test "$base0" = "$base1" || {
		echo >&2 "BAD: stale $base in $target"
		exit 2
	}

	cd ../git.one || exit 1
	if grep "$merge" :basecheck-tested-ok >/dev/null
	then
		exit 0
	elif grep "$merge" :basecheck-tested-ng >/dev/null
	then
		echo >&2 "BAD (again): $label"
		exit 1
	fi

	echo >&2 "Testing $merge $target"
	git reset --quiet --hard "$merge" || exit 1
	if Meta/Make -s -j32 >:basecheck-errors 2>&1
	then
		echo >&2 "OK: $label"
		echo "$merge" >>:basecheck-tested-ok
		exit 0
	else
		echo "$merge" >>:basecheck-tested-ng
		cat ":basecheck-errors"
		echo >&2 "BAD: $label"
		exit 1
	fi

	exit 0 ;# just in case
fi

git log --oneline --abbrev=-1 --min-parents=2 ..seen |
sed -n -e "s|^\([0-9a-f]*\) Merge branch '\(.*\)' into \(../..*\)$|\1 \2 \3|p" |
{
	exit=
	while read merge base target
	do
		"$0" "$merge" "$base" "$target" </dev/null || exit=$?
	done
	exit $exit
}
