#!/bin/sh

test_description='git apply with core.ignorecase'

. ./test-lib.sh

test_expect_success setup '
       # initial commit has file0 only
       test_commit "initial" file0 "initial commit with file0" initial &&

       # current commit has file1 as well
       test_commit "current" file1 "initial content of file1" current &&
       file0blob=$(git rev-parse :file0) &&
       file1blob=$(git rev-parse :file1) &&

       # prepare sample patches
       # file0 is modified
       echo modification to file0 >file0 &&
       git add file0 &&
       modifiedfile0blob=$(git rev-parse :file0) &&

       # file1 is removed and then ...
       git rm --cached file1 &&
       # ... identical copies are placed at File1 and file2
       git update-index --add --cacheinfo 100644,$file1blob,file2 &&
       git update-index --add --cacheinfo 100644,$file1blob,File1 &&

       # then various patches to do basic things
       git diff HEAD^ HEAD -- file1 >creation-patch &&
       git diff HEAD HEAD^ -- file1 >deletion-patch &&
       git diff --cached HEAD -- file1 file2 >rename-file1-to-file2-patch &&
       git diff --cached HEAD -- file1 File1 >rename-file1-to-File1-patch &&
       git diff --cached HEAD -- file0 >modify-file0-patch &&

       # then set up for swap
       git reset --hard current &&
       test_commit "swappable" file3 "different content for file3" swappable &&
       file3blob=$(git rev-parse :file3) &&
       git rm --cached file1 file3 &&
       git update-index --add --cacheinfo 100644,$file1blob,File3 &&
       git update-index --add --cacheinfo 100644,$file3blob,File1 &&
       git diff --cached HEAD -- file1 file3 File1 File3 >swap-file1-and-file3-to-File3-and-File1-patch
'

# Basic creation, deletion, modification and renaming.
test_expect_success 'creation and deletion' '
       # start at "initial" with file0 only
       git reset --hard initial &&

       # add file1
       git -c core.ignorecase=false apply --cached creation-patch &&
       test_cmp_rev :file1 "$file1blob" &&

       # remove file1
       git -c core.ignorecase=false apply --cached deletion-patch &&
       test_must_fail git rev-parse --verify :file1 &&

       # do the same with ignorecase
       git -c core.ignorecase=true apply --cached creation-patch &&
       test_cmp_rev :file1 "$file1blob" &&
       git -c core.ignorecase=true apply --cached deletion-patch &&
       test_must_fail git rev-parse --verify :file1
'

test_expect_success 'modification (index-only)' '
       # start at "initial" with file0 only
       git reset --hard initial &&

       # modify file0
       git -c core.ignorecase=false apply --cached modify-file0-patch &&
       test_cmp_rev :file0 "$modifiedfile0blob" &&
       git -c core.ignorecase=false apply --cached -R modify-file0-patch &&
       test_cmp_rev :file0 "$file0blob" &&

       # do the same with ignorecase
       git -c core.ignorecase=true apply --cached modify-file0-patch &&
       test_cmp_rev :file0 "$modifiedfile0blob" &&
       git -c core.ignorecase=true apply --cached -R modify-file0-patch &&
       test_cmp_rev :file0 "$file0blob"
'

test_expect_success 'rename file1 to file2 (index-only)' '
       # start from file0 and file1
       git reset --hard current &&

       # rename file1 to file2
       git -c core.ignorecase=false apply --cached rename-file1-to-file2-patch &&
       test_must_fail git rev-parse --verify :file1 &&
       test_cmp_rev :file2 "$file1blob" &&
       git -c core.ignorecase=false apply --cached -R rename-file1-to-file2-patch &&
       test_must_fail git rev-parse --verify :file2 &&
       test_cmp_rev :file1 "$file1blob" &&

       # do the same with ignorecase
       git -c core.ignorecase=true apply --cached rename-file1-to-file2-patch &&
       test_must_fail git rev-parse --verify :file1 &&
       test_cmp_rev :file2 "$file1blob" &&
       git -c core.ignorecase=true apply --cached -R rename-file1-to-file2-patch &&
       test_must_fail git rev-parse --verify :file2 &&
       test_cmp_rev :file1 "$file1blob"
'

test_expect_success 'rename file1 to File1 (index-only)' '
       # start from file0 and file1
       git reset --hard current &&

       # rename file1 to File1
       git -c core.ignorecase=false apply --cached rename-file1-to-File1-patch &&
       test_must_fail git rev-parse --verify :file1 &&
       test_cmp_rev :File1 "$file1blob" &&
       git -c core.ignorecase=false apply --cached -R rename-file1-to-File1-patch &&
       test_must_fail git rev-parse --verify :File1 &&
       test_cmp_rev :file1 "$file1blob" &&

       # do the same with ignorecase
       git -c core.ignorecase=true apply --cached rename-file1-to-File1-patch &&
       test_must_fail git rev-parse --verify :file1 &&
       test_cmp_rev :File1 "$file1blob" &&
       git -c core.ignorecase=true apply --cached -R rename-file1-to-File1-patch &&
       test_must_fail git rev-parse --verify :File1 &&
       test_cmp_rev :file1 "$file1blob"
'

# involve filesystem on renames
test_expect_success 'rename file1 to File1 (with ignorecase, working tree)' '
       # start from file0 and file1
       git reset --hard current &&

       # do the same with ignorecase
       git -c core.ignorecase=true apply --index rename-file1-to-File1-patch &&
       test_must_fail git rev-parse --verify :file1 &&
       test_cmp_rev :File1 "$file1blob" &&
       git -c core.ignorecase=true apply --index -R rename-file1-to-File1-patch &&
       test_must_fail git rev-parse --verify :File1 &&
       test_cmp_rev :file1 "$file1blob"
'

test_expect_success CASE_INSENSITIVE_FS 'rename file1 to File1 (without ignorecase, case-insensitive FS)' '
       # start from file0 and file1
       git reset --hard current &&

       # rename file1 to File1 without ignorecase (fails as expected)
       test_must_fail git -c core.ignorecase=false apply --index rename-file1-to-File1-patch &&
       git rev-parse --verify :file1 &&
       test_cmp_rev :file1 "$file1blob"
'

test_expect_success !CASE_INSENSITIVE_FS 'rename file1 to File1 (without ignorecase, case-sensitive FS)' '
       # start from file0 and file1
       git reset --hard current &&

       # rename file1 to File1 without ignorecase
       git -c core.ignorecase=false apply --index rename-file1-to-File1-patch &&
       test_must_fail git rev-parse --verify :file1 &&
       test_cmp_rev :File1 "$file1blob" &&
       git -c core.ignorecase=false apply --index -R rename-file1-to-File1-patch &&
       test_must_fail git rev-parse --verify :File1 &&
       test_cmp_rev :file1 "$file1blob"
'

test_expect_success 'rename file1 to file2 with working tree conflict' '
       # start from file0 and file1, and file2 untracked
       git reset --hard current &&
       test_when_finished "rm file2" &&
       touch file2 &&

       # rename file1 to file2 with conflict
       test_must_fail git -c core.ignorecase=false apply --index rename-file1-to-file2-patch &&
       git rev-parse --verify :file1 &&
       test_cmp_rev :file1 "$file1blob" &&

       # do the same with ignorecase
       test_must_fail git -c core.ignorecase=true apply --index rename-file1-to-file2-patch &&
       git rev-parse --verify :file1 &&
       test_cmp_rev :file1 "$file1blob"
'

test_expect_success 'rename file1 to file2 with case-insensitive conflict (index-only - ignorecase disabled)' '
       # start from file0 and file1, and File2 in index
       git reset --hard current &&
       git update-index --add --cacheinfo 100644,$file3blob,File2 &&

       # rename file1 to file2 without ignorecase
       git -c core.ignorecase=false apply --cached rename-file1-to-file2-patch &&
       test_must_fail git rev-parse --verify :file1 &&
       test_cmp_rev :file2 "$file1blob" &&
       git -c core.ignorecase=false apply --cached -R rename-file1-to-file2-patch &&
       test_must_fail git rev-parse --verify :file2 &&
       test_cmp_rev :file1 "$file1blob" &&
       test_cmp_rev :File2 "$file3blob"
'

test_expect_failure 'rename file1 to file2 with case-insensitive conflict (index-only - ignorecase enabled)' '
       # start from file0 and file1, and File2 in index
       git reset --hard current &&
       git update-index --add --cacheinfo 100644,$file3blob,File2 &&

       # rename file1 to file2 with ignorecase, with a "File2" conflicting file in place - expect failure.
       # instead of failure, we get success with "File1" and "file1" both existing in the index, despite
       # the ignorecase configuration.
       test_must_fail git -c core.ignorecase=true apply --cached rename-file1-to-file2-patch &&
       git rev-parse --verify :file1 &&
       test_cmp_rev :file1 "$file1blob" &&
       test_cmp_rev :File2 "$file3blob"
'

test_expect_success 'rename file1 to File1 with case-sensitive conflict (index-only)' '
       # start from file0 and file1, and File1 in index
       git reset --hard current &&
       git update-index --add --cacheinfo 100644,$file3blob,File1 &&

       # On a case-insensitive filesystem with core.ignorecase on, a single git
       # "reset --hard" will actually leave things wrong because of the
       # index-to-working-tree discrepancy - see "reset --hard handles
       # index-only case-insensitive duplicate" under t7104-reset-hard.sh.
       # We are creating this unexpected state, so we should explicitly queue
       # an extra reset. If reset ever starts to handle this case, this will
       # become unnecessary but also not harmful.
       test_when_finished "git reset --hard" &&

       # rename file1 to File1 when File1 is already in index (fails with conflict)
       test_must_fail git -c core.ignorecase=false apply --cached rename-file1-to-File1-patch &&
       git rev-parse --verify :file1 &&
       test_cmp_rev :file1 "$file1blob" &&
       test_cmp_rev :File1 "$file3blob" &&

       # do the same with ignorecase
       test_must_fail git -c core.ignorecase=true apply --cached rename-file1-to-File1-patch &&
       git rev-parse --verify :file1 &&
       test_cmp_rev :file1 "$file1blob" &&
       test_cmp_rev :File1 "$file3blob"
'

test_expect_success CASE_INSENSITIVE_FS 'case-insensitive swap - file1 to File2 and file2 to File1 (working tree)' '
       # start from file0, file1, and file3
       git reset --hard swappable &&

       # "swap" file1 and file3 to case-insensitive versions without ignorecase on case-insensitive FS (fails as expected)
       test_must_fail git -c core.ignorecase=false apply --index swap-file1-and-file3-to-File3-and-File1-patch &&
       git rev-parse --verify :file1 &&
       git rev-parse --verify :file3 &&
       test_cmp_rev :file1 "$file1blob" &&
       test_cmp_rev :file3 "$file3blob" &&

       # do the same with ignorecase
       git -c core.ignorecase=true apply --index swap-file1-and-file3-to-File3-and-File1-patch &&
       test_must_fail git rev-parse --verify :file1 &&
       test_must_fail git rev-parse --verify :file3 &&
       test_cmp_rev :File3 "$file1blob" &&
       test_cmp_rev :File1 "$file3blob" &&
       git -c core.ignorecase=true apply --index -R swap-file1-and-file3-to-File3-and-File1-patch &&
       test_must_fail git rev-parse --verify :File1 &&
       test_must_fail git rev-parse --verify :File3 &&
       test_cmp_rev :file1 "$file1blob" &&
       test_cmp_rev :file3 "$file3blob"
'

test_done
