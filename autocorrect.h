#ifndef AUTOCORRECT_H
#define AUTOCORRECT_H

/* An empirically derived magic number */
#define AUTOCORRECT_SIMILARITY_FLOOR 7
#define AUTOCORRECT_SIMILAR_ENOUGH(x) ((x) < AUTOCORRECT_SIMILARITY_FLOOR)

enum autocorrect_mode {
	AUTOCORRECT_HINT,
	AUTOCORRECT_NEVER,
	AUTOCORRECT_PROMPT,
	AUTOCORRECT_IMMEDIATELY,
	AUTOCORRECT_DELAY,
};

struct autocorrect {
	enum autocorrect_mode mode;
	int delay;
};

void autocorrect_resolve(struct autocorrect *conf);

void autocorrect_confirm(struct autocorrect *conf, const char *assumed);

#endif /* AUTOCORRECT_H */
