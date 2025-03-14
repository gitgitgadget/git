#!/bin/sh

make CHECK_ASSERTION_SIDE_EFFECTS=1 >compiler_output 2>compiler_error
if test $? != 0
then
    echo "ERROR: The compiler could not verify the following assert()" >&2
    echo "       calls are free of side-effects.  Please replace with" >&2
    echo "       BUG_IF_NOT() calls." >&2
    grep undefined.reference.to..not_supposed_to_survive compiler_error \
      | sed -e s/:[^:]*$// | sort | uniq | tr ':' ' ' \
      | while read f l
      do
	printf "${f}:${l}\n  "
	awk -v start="$l" 'NR >= start { print; if (/\);/) exit }' $f
      done
    exit 1
fi
rm compiler_output compiler_error
