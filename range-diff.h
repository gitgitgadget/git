#ifndef BRANCH_DIFF_H
#define BRANCH_DIFF_H

#include "diff.h"

int show_range_diff(const char *range1, const char *range2,
		    int creation_factor, struct diff_options *diffopt);

#endif
