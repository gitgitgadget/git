#include <pthread.h>
#include "../git-compat-util.h"
/* We need original mmap/munmap here. */
#undef mmap
#undef munmap

/*
 * OSX doesn't have any specific setting like Linux's vm.max_map_count,
 * so COUNT_MAX can be any large number. We here set it to the default
 * value of Linux's vm.max_map_count.
 */
#define COUNT_MAX (65530)

struct munmap_queue {
	void *start;
	size_t length;
};

void *git_mmap(void *start, size_t length, int prot, int flags, int fd, off_t offset)
{
	/*
	 * We can simply discard munmap operations in the queue by
	 * restricting mmap arguments.
	 */
	if (start != NULL || flags != MAP_PRIVATE || prot != PROT_READ)
		die("invalid usage of mmap");
	return mmap(start, length, prot, flags, fd, offset);
}

int git_munmap(void *start, size_t length)
{
	static pthread_mutex_t mutex;
	static struct munmap_queue *queue;
	static int count;
	int i;

	pthread_mutex_lock(&mutex);
	if (!queue)
		queue = xmalloc(COUNT_MAX * sizeof(struct munmap_queue));
	queue[count].start = start;
	queue[count].length = length;
	if (++count == COUNT_MAX) {
		for (i = 0; i < COUNT_MAX; i++)
			munmap(queue[i].start, queue[i].length);
		count = 0;
	}
	pthread_mutex_unlock(&mutex);
	return 0;
}
