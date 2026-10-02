#!/bin/sh
#
# git-son: create an independent child repository that knows its parent
#

SUBDIRECTORY_OK='Yes'
OPTIONS_SPEC='git son [options] <name>
--
inherit    fetch parent history into the son
branch=    start the son from a specific parent branch
'

. git-sh-setup
require_work_tree
cd_to_toplevel

inherit=
branch=
while test $# -gt 0
do
	case "$1" in
	--inherit)
		inherit=1 ;;
	--branch)
		shift
		branch="$1" ;;
	--)
		shift; break ;;
	-*)
		usage ;;
	*)
		break ;;
	esac
	shift
done

name="$1"
test -n "$name" || usage

if test -n "$branch" && test -z "$inherit"
then
	die "fatal: --branch requires --inherit"
fi

parent_dir="$(pwd)"
parent_remote="$(git remote get-url origin 2>/dev/null)" || parent_remote=

if test -e "$name"
then
	die "fatal: '$name' already exists"
fi

mkdir "$name" || die "fatal: could not create directory '$name'"

if ! echo "$name/" >> "$parent_dir/.gitignore" 2>/dev/null
then
	rm -rf "$name"
	die "fatal: could not update .gitignore"
fi

cd "$name" || die "fatal: could not enter directory '$name'"

if ! git init
then
	rm -rf "$parent_dir/$name"
	die "fatal: could not initialize repository in '$name'"
fi

if test -n "$parent_remote"
then
	git remote add parent "$parent_remote"
else
	git remote add parent "$parent_dir"
fi

if test -n "$inherit"
then
	git fetch parent || die "fatal: could not fetch from parent"
	if test -n "$branch"
	then
		git checkout -b "$branch" "parent/$branch" ||
			die "fatal: could not checkout branch '$branch'"
	else
		git checkout -b main parent/HEAD 2>/dev/null ||
			git checkout -b main "parent/$(git remote show parent | sed -n 's/.*HEAD branch: //p')" 2>/dev/null ||
			echo "warning: could not determine parent HEAD, starting empty"
	fi
else
	echo "# $name" > README.md
	git add README.md
	git commit -q -m "Initial commit"
fi

echo ""
echo "Created son repository '$name'"
echo "  parent: ${parent_remote:-$parent_dir}"
echo "  inherit: ${inherit:-no}"
