#ifndef PRETTY_LIB_H
#define PRETTY_LIB_H

/**
 * This is a possibly temporary interface between
 * ref-filter and pretty. This interface may disappear in the
 * future if a way to use ref-filter directly is found.
 * In the meantime, this interface would enable us to
 * step by step replace the formatting code in pretty by the
 * ref-filter code.
*/

/**
 * Possible future replacement for "pretty_print_commit()".
 * Uses ref-filter's logic.
*/
void ref_pretty_print_commit(struct pretty_print_context *pp,
			const struct commit *commit,
			struct strbuf *sb);

#endif /* PRETTY_LIB_H */
