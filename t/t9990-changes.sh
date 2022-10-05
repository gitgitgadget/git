#!/bin/sh

test_description='git change - low level meta-commit management'

. ./test-lib.sh

. "$TEST_DIRECTORY"/lib-rebase.sh

test_expect_success 'setup commits and meta-commits' '
  for c in one two three
  do
    test_commit $c &&
    git change update --content $c >actual 2>err &&
    echo "Created change metas/$c" >expect &&
    test_cmp expect actual &&
    test_must_be_empty err &&
    test_cmp_rev refs/metas/$c $c || return 1
  done
'

# Check a meta-commit has the correct parents Call with the object
# name of the meta-commit followed by pairs of type and parent
check_meta_commit () {
  name=$1
  shift
  while test $# -gt 0
  do
    printf '%s %s\n' $1 $(git rev-parse --verify $2)
    shift
    shift
  done | sort >expect
  git cat-file commit $name >metacommit &&
  # commit body should consist of parent-type
  types="$(sed -n '/^$/ {
    :loop
    n
    s/^parent-type //
    p
    b loop
  }' metacommit)" &&
  while read key value
  do
    # TODO: don't sort the first parent
    if test "$key" = "parent"
    then
      type="${types%% *}"
      test -n "$type" || return 1
      printf '%s %s\n' $type $value
      types="${types#?}"
      types="${types# }"
    elif test "$key" = "tree"
    then
      test_cmp_rev "$value" $EMPTY_TREE || return 1
    elif test -z "$key"
    then
      # only parse commit headers
      break
    fi
  done <metacommit >actual-unsorted &&
  test -z "$types" &&
  sort >actual <actual-unsorted &&
  test_cmp expect actual
}

test_expect_success 'update meta-commits after rebase' '
  (
    set_fake_editor &&
    FAKE_AMEND=edited &&
    FAKE_LINES="reword 1 pick 2 fixup 3" &&
    export FAKE_AMEND FAKE_LINES &&
    git rebase -i --root
  ) &&

  # update meta-commits
  git change update --replace tags/one --content HEAD~1 >out 2>err &&
  echo "Updated change metas/one" >expect &&
  test_cmp expect out &&
  test_must_be_empty err &&
  git change update --replace tags/two --content HEAD@{2} &&
  oid=$(git rev-parse --verify metas/two) &&
  git change update --replace HEAD@{2} --replace tags/three --content HEAD &&
  # check meta-commits
  check_meta_commit metas/one c HEAD~1 r tags/one &&
  check_meta_commit $oid c HEAD@{2} r tags/two &&
  # NB this checks that "git change update" uses the meta-commit ($oid)
  #    corresponding to the replaces commit (HEAD@2 above) given on the
  #    commandline.
  check_meta_commit metas/two c HEAD r $oid r tags/three &&
  check_meta_commit metas/three c HEAD r $oid r tags/three
'

reset_meta_commits () {
    for c in one two three
    do
       echo "update refs/metas/$c refs/tags/$c^0"
    done | git update-ref --stdin
}

test_expect_success 'override change name' '
  # TODO: builtin/change.c expects --change to be the full refname,
  #       ideally it would prepend refs/metas to the string given by the
  #       user.
  git change update --change refs/metas/another-one --content one &&
  test_cmp_rev metas/another-one one
'

test_expect_success 'non-fast forward meta-commit update refused' '
  test_must_fail git change update --change refs/metas/one --content two \
    >out 2>err &&
  echo "error: non-fast-forward update to ${SQ}refs/metas/one${SQ}" \
    >expect &&
  test_cmp expect err &&
  test_must_be_empty out
'

test_expect_success 'forced non-fast forward update succeeds' '
  git change update --change refs/metas/one --content two --force \
    >out 2>err &&
  echo "Updated change metas/one" >expect &&
  test_cmp expect out &&
  test_must_be_empty err
'

test_expect_success 'list changes' '
  cat >expect <<-\EOF &&
metas/another-one
metas/one
metas/three
metas/two
EOF
  git change list >actual &&
  test_cmp expect actual
'

test_expect_success 'delete change' '
  git change delete metas/one &&
  cat >expect <<-\EOF &&
metas/another-one
metas/three
metas/two
EOF
  git change list >actual &&
  test_cmp expect actual
'

test_done
