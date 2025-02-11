//
// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

// POSIX threads.

#include "core/nng_impl.h"
#include "nng/nng.h"

#ifdef NNG_PLATFORM_DEOS

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static bool nni_plat_inited = false;

pthread_condattr_t  nni_cvattr;
pthread_mutexattr_t nni_mxattr;
pthread_attr_t      nni_thrattr;

void
nni_plat_mtx_init(nni_plat_mtx *mtx)
{
	pthread_mutex_init(&mtx->mtx, &nni_mxattr);
}

void
nni_plat_mtx_fini(nni_plat_mtx *mtx)
{
	(void) pthread_mutex_destroy(&mtx->mtx);
}

static void
nni_pthread_mutex_lock(pthread_mutex_t *m)
{
	(void) pthread_mutex_lock(m);
}

static void
nni_pthread_mutex_unlock(pthread_mutex_t *m)
{
	(void) pthread_mutex_unlock(m);
}

static void
nni_pthread_cond_broadcast(pthread_cond_t *c)
{
	(void) pthread_cond_broadcast(c);
}

static void
nni_pthread_cond_signal(pthread_cond_t *c)
{
	(void) pthread_cond_signal(c);
}

static void
nni_pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
	(void) pthread_cond_wait(c, m);
}

static int
nni_pthread_cond_timedwait(
    pthread_cond_t *c, pthread_mutex_t *m, struct timespec *ts)
{
	int rv;

	switch ((rv = pthread_cond_timedwait(c, m, ts))) {
	case 0:
		return (0);
	case ETIMEDOUT:
	case EAGAIN:
		return (NNG_ETIMEDOUT);
	}
	return (NNG_EINVAL);
}

void
nni_plat_mtx_lock(nni_plat_mtx *mtx)
{
	nni_pthread_mutex_lock(&mtx->mtx);
}

void
nni_plat_mtx_unlock(nni_plat_mtx *mtx)
{
	nni_pthread_mutex_unlock(&mtx->mtx);
}

void
nni_plat_cv_init(nni_plat_cv *cv, nni_plat_mtx *mtx)
{
	// See the comments in nni_plat_mtx_init.  Almost everywhere this
	// simply does not/cannot fail.

	while (pthread_cond_init(&cv->cv, &nni_cvattr) != 0) {
		nni_msleep(10);
	}
	cv->mtx = &mtx->mtx;
}

void
nni_plat_cv_wake(nni_plat_cv *cv)
{
	nni_pthread_cond_broadcast(&cv->cv);
}

void
nni_plat_cv_wake1(nni_plat_cv *cv)
{
	nni_pthread_cond_signal(&cv->cv);
}

void
nni_plat_cv_wait(nni_plat_cv *cv)
{
	nni_pthread_cond_wait(&cv->cv, cv->mtx);
}

int
nni_plat_cv_until(nni_plat_cv *cv, nni_time until)
{
	struct timespec ts;

	// Our caller has already guaranteed a sane value for until.
	ts.tv_sec  = until / 1000;
	ts.tv_nsec = (until % 1000) * 1000000;

	return (nni_pthread_cond_timedwait(&cv->cv, cv->mtx, &ts));
}

void
nni_plat_cv_fini(nni_plat_cv *cv)
{
	(void) pthread_cond_destroy(&cv->cv);
	cv->mtx = NULL;
}

static void *
nni_plat_thr_main(void *arg)
{
	nni_plat_thr *thr = arg;
	sigset_t      set;

	// Suppress (block) SIGPIPE for this thread.
	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	(void) pthread_sigmask(SIG_BLOCK, &set, NULL);

	thr->func(thr->arg);
	return (NULL);
}

int
nni_plat_thr_init(nni_plat_thr *thr, void (*fn)(void *), void *arg)
{
	int rv;

	thr->func = fn;
	thr->arg  = arg;

	// POSIX wants functions to return a void *, but we don't care.
	rv = pthread_create(&thr->tid, &nni_thrattr, nni_plat_thr_main, thr);
	if (rv != 0) {
		return (NNG_ENOMEM);
	}
	return (0);
}

void
nni_plat_thr_fini(nni_plat_thr *thr)
{
	pthread_join(thr->tid, NULL);
}

bool
nni_plat_thr_is_self(nni_plat_thr *thr)
{
	return (pthread_self() == thr->tid);
}

void
nni_plat_thr_set_name(nni_plat_thr *thr, const char *name)
{
	NNI_ARG_UNUSED(thr);
	NNI_ARG_UNUSED(name);
}

int
nni_plat_init(nng_init_params *params)
{
	int rv = 0;

	if ((pthread_mutexattr_init(&nni_mxattr) != 0) ||
	    (pthread_condattr_init(&nni_cvattr) != 0) ||
	    (pthread_attr_init(&nni_thrattr) != 0)) {
		// Technically this is leaking, but it should never
		// occur, so really not worried about it.
		return (NNG_ENOMEM);
	}

	if (pthread_condattr_setclock(&nni_cvattr, CLOCK_MONOTONIC) != 0) {
		nni_plat_fini();
		return (NNG_ENOMEM);
	}

	nni_udp_init(NULL);

	nni_plat_inited = true;

	return (rv);
}

void
nni_plat_fini(void)
{
	// if (nni_plat_inited) {
	// 	nni_posix_resolv_sysfini();
	// }
	pthread_mutexattr_destroy(&nni_mxattr);
	pthread_condattr_destroy(&nni_cvattr);
	pthread_attr_destroy(&nni_thrattr);
	nni_plat_inited = false;
}

int
nni_plat_ncpu(void)
{
	// Assume only a single CPU for this platform.
	// (Uder extended we could use sysconf.)
	return (1);
}

#endif // NNG_PLATFORM_POSIX
