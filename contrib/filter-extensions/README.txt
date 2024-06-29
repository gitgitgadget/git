= List-Objects-Filter Extensions API
:pp: {plus}{plus}

This API can be used to develop filter extensions used for custom filtering
behaviour with `git-upload-pack` and `git-rev-list`. The API is defined in
link:../../list-objects-filter-extensions.h[list-objects-filter-extensions.h]
and defines three functions to implement a filter operation.

NOTE: Each filter implementing this API must compiled into Git as a
static library. There is some plumbing in the Makefile to help with this
via `FILTER_EXTENSIONS`.

== Overview

. You write a filter and compile it into your custom build of git.
. A filter request is received that specifically names the filter extension
that you have written, ie: `--filter=extension:<name>[=<arg>]`
. The `init_fn` function of your filter is called.
. The `filter_object_fn` function of your filter is called for each object
at least once.
. The `free_fn` function of your filter is called.

== Examples

*link:./rand/[`rand`]* is a filter that matches all trees and a random
percentage of blobs, where the percentage is parsed from the filter arg. It
imports and uses the `oid_to_hex()` and `trace_key_printf()` functions from the
Git API.

Build via:

[,console]
----
$ make FILTER_EXTENSIONS=contrib/filter-extensions/rand/rand.a
    ...
    SUBDIR contrib/filter-extensions/rand
    ...
----

We can run against git's own repo:

[,console]
----
$ ./git rev-list refs/heads/master --objects --max-count 1 --filter=extension:rand=3 --filter-print-omitted | grep -c '^~'
filter-rand: matching 3%
filter-rand: done: count=4068 (blob=3866 tree=202) matched=117 elapsed=0.005017s rate=810843.1/s average=1.2us
3749  # number of omitted blobs = 3866 - 117
----

== Development

See the examples for a basic implementation. The comments in
link:../../list-objects-filter.h[`list-objects-filter.h`] and the built-in
filter implementations in
link:../../list-objects-filter.c[`list-objects-filter.c`] are important to
understand how filters are implemented - `filter_blobs_limit()` provides a
simple example, and `filter_sparse()` is more complex.

The API differences between the built-in filters and the filter extensions:

. Filter extensions don't handle ``omitset``s directly, instead setting `omit`.
. Filter extensions receive a void pointer they can use for context.

== Building

There is some plumbing in the Git Makefile to help with this via
`FILTER_EXTENSIONS`, setting it to space-separated paths of the filter extension
static libraries indicates that these filters should be compiled into git.
For example:

[,console]
----
make FILTER_EXTENSIONS=contrib/filter-extensions/rand/rand.a
----

Filter extensions don't need to be within the Git source tree. A filter
extension static library should either exist at the given path - ie, `rand.a`
should exist - or there should be a Makefile in that directory which will create
it when `make rand.a` is run. (Such a Makefile should also have a `clean` target
which deletes all object files and brings the directory back to its initial
state).

The static library should define a struct of type `filter_extension` called
`filter_extension_NAME` where `NAME` is the name of your extension (ie `rand`
for `rand.a`). See
link:../../list-objects-filter-extensions.h[list-objects-filter-extensions.h]

This definition should follow the following pattern:

[,C]
----
#include "list-objects-filter-extensions.h"

/* Definitions of rand_init, rand_filter_object, rand_free ... */

const struct filter_extension filter_extension_rand = {
    "rand",
    &rand_init,
    &rand_filter_object,
    &rand_free,
};
----

(The names of your `init_fn`, `filter_object_fn` and `free_fn` are not
important, but the string literal should again be the the name of your extension
- `"rand"` for the filter extension in `rand.a`.)

You may use library functions from Git if you include the relevant Git headers,
since the filter extensions and Git itself will be linked together into a single
binary.

You may depend on other libraries if you indicate that they are to be linked
into the Git binary using `LDFLAGS`. See the C{pp} example below.

== Developing in C{pp} (and other languages)

You can develop filter extensions with C{pp}, but many Git header files are not
compatible with modern C{pp}, so you won't be able to directly use Git library
functions. However, you can use them if you create wrapper functions in C that
delegates to the Git library functions you need, but which are also C{pp}
compatible. See link:./rand_cpp/[`rand_cpp`] for a simple example. A similar
solution would be to implement the extension itself in C, and have the
extension do any operations that require Git library functions, but have it
delegate to a C wrapper API that you add to a C{pp} library that already
contains the domain-specific operations that you need. In either case, remember
to wrap any functions that must be C-compatible with `extern C` when declaring
or defining them from within C{pp}.

To build the C{pp} example:

[,console]
----
make FILTER_EXTENSIONS=contrib/filter-extensions/rand_cpp/rand_cpp.a \
     LDFLAGS=-lstdc++
----

For other languages you'll either need to port definitions of some internal Git
structs (at a minimum, `object`, `object_id`, `repository`, and `hash_algo`) -
or again, you could write the extension in C but have it delegate to a domain
specific library in the language of your choice that has a C-compatible API.
Extra libraries can be required using `LDFLAGS`.

== Linking more than one filter extension

To link in more than one extension, set `FILTER_EXTENSIONS` to the
space-separated paths of all the extensions you want linked. For example, to
link in both example filters at once:

[,console]
----
make FILTER_EXTENSIONS="contrib/filter-extensions/rand/rand.a contrib/filter-extensions/rand_cpp/rand_cpp.a" \
     LDFLAGS=-lstdc++
----
