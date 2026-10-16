#include "git-compat-util.h"
#include "sigchain.h"

#define SIGCHAIN_MAX_SIGNALS 32

struct sigchain_signal {
	sigchain_fun *old;
	int n;
	int alloc;
	/*
	 * Serializes n/old updates.  Two threads can be inside the handler
	 * for the same signal at the same time (e.g. two copies delivered
	 * before the first handler finished), and both call
	 * sigchain_pop(); without this lock both read the same s->n and
	 * both install s->old[s->n - 1], so the chain unwinds one entry
	 * too many while the dispatcher keeps pointing at a popped
	 * handler.
	 */
	volatile sig_atomic_t lock;
};
static struct sigchain_signal signals[SIGCHAIN_MAX_SIGNALS];

static void check_signum(int sig)
{
	if (sig < 1 || sig >= SIGCHAIN_MAX_SIGNALS)
		BUG("signal out of range: %d", sig);
}

/*
 * The lock must be taken with the signal blocked: a second delivery of
 * the same signal to this thread would otherwise try to take the lock
 * from the handler and deadlock against the interrupted lock holder.
 * The mask is restored once the update is done, so a pending copy of the
 * signal is simply delivered after push/pop returns.
 */
#ifdef GIT_WINDOWS_NATIVE
static void chain_block_signal(int sig, sigset_t *saved)
{
	/* console control handlers run on a separate thread */
	(void)sig;
	(void)saved;
}

static void chain_restore_mask(const sigset_t *saved)
{
	(void)saved;
}

static void chain_lock(volatile sig_atomic_t *l)
{
	while (InterlockedExchange((LONG volatile *)l, 1))
		; /* spin */
}

static void chain_unlock(volatile sig_atomic_t *l)
{
	InterlockedExchange((LONG volatile *)l, 0);
}
#else
static void chain_block_signal(int sig, sigset_t *saved)
{
	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, sig);
	sigprocmask(SIG_BLOCK, &mask, saved);
}

static void chain_restore_mask(const sigset_t *saved)
{
	sigprocmask(SIG_SETMASK, saved, NULL);
}

static void chain_lock(volatile sig_atomic_t *l)
{
	while (__sync_lock_test_and_set(l, 1))
		; /* spin */
}

static void chain_unlock(volatile sig_atomic_t *l)
{
	__sync_lock_release(l);
}
#endif

int sigchain_push(int sig, sigchain_fun f)
{
	struct sigchain_signal *s = signals + sig;
	sigset_t saved;
	int ret = 0;

	check_signum(sig);

	chain_block_signal(sig, &saved);
	chain_lock(&s->lock);

	ALLOC_GROW(s->old, s->n + 1, s->alloc);
	s->old[s->n] = signal(sig, f);
	if (s->old[s->n] == SIG_ERR)
		ret = -1;
	else
		s->n++;

	chain_unlock(&s->lock);
	chain_restore_mask(&saved);
	return ret;
}

int sigchain_pop(int sig)
{
	struct sigchain_signal *s = signals + sig;
	sigset_t saved;
	int ret = 0;

	check_signum(sig);

	chain_block_signal(sig, &saved);
	chain_lock(&s->lock);

	if (s->n < 1)
		; /* empty chain; keep reporting success */
	else if (signal(sig, s->old[s->n - 1]) == SIG_ERR)
		ret = -1;
	else
		s->n--;

	chain_unlock(&s->lock);
	chain_restore_mask(&saved);
	return ret;
}

void sigchain_push_common(sigchain_fun f)
{
	sigchain_push(SIGINT, f);
	sigchain_push(SIGHUP, f);
	sigchain_push(SIGTERM, f);
	sigchain_push(SIGQUIT, f);
	sigchain_push(SIGPIPE, f);
}

void sigchain_pop_common(void)
{
	sigchain_pop(SIGPIPE);
	sigchain_pop(SIGQUIT);
	sigchain_pop(SIGTERM);
	sigchain_pop(SIGHUP);
	sigchain_pop(SIGINT);
}
