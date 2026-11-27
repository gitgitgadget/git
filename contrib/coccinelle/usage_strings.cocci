@ usage_strings @
identifier opts;
constant opt_flag != OPT_GROUP;
char[] e;
@@



struct option opts[] = {
    ...,
    opt_flag(...,<+... \(N_(e)\|e\) ...+>, ...),
    ...,};


@script:python string_checker depends on usage_strings@
e << usage_strings.e;
replacement;
@@

length = len(e)
should_make_change = False
if length > 2:
    if e[length-2] == '.' and e[length-3] != '.':
        coccinelle.replacement = e[:length-2] + '"'
        should_make_change = True
    else:
        coccinelle.replacement = e
    if e[1].isupper():
        if not e[2].isupper():
            coccinelle.replacement = coccinelle.replacement[0] + coccinelle.replacement[1].lower() + coccinelle.replacement[2:]
            should_make_change = True
if not should_make_change:
    cocci.include_match(False)

@ depends on string_checker@
identifier usage_strings.opts;
constant usage_strings.opt_flag;
char[] usage_strings.e;
identifier string_checker.replacement;
@@

struct option opts[] = {
    ...,
    opt_flag(...,
    \(
-    N_(e)
+    N_(replacement)
    \|
-    e
+    replacement
    \)
    , ...),
    ...,};
