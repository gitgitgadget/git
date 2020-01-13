#!/bin/sh
#
# Build and test Git
#

. ${0%/*}/lib.sh

case "$CI_OS_NAME" in
windows*) cmd //c mklink //j t\\.prove "$(cygpath -aw "$cache_dir/.prove")";;
*) ln -s "$cache_dir/.prove" t/.prove;;
esac

make

cd t
GIT_TEST_ADD_I_USE_BUILTIN=1 ./t3701-add-interactive.sh -r 39,49 -v -x -i

