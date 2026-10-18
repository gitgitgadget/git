#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

static pid_t fork_returns_enosys(void)
{
	errno = ENOSYS;
	return -1;
}

__attribute__((used)) static struct {
	pid_t (*replacement)(void);
	pid_t (*replacee)(void);
} interpose __attribute__((section("__DATA,__interpose"))) = {
	fork_returns_enosys,
	fork,
};
