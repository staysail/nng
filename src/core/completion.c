//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"

#include <stdlib.h>
#include <string.h>

struct nni_cq {
	nni_list	cq_ents;
	int		cq_close;
	nni_mtx		cq_mtx;
	nni_list_node	cq_node;        // On global list of CQs.
};

int
nni_cq_init(nni_cq **cqp)
{
	nni_cq *cq;
	int rv;

	if ((cq = NNI_ALLOC_STRUCT(cq)) == NULL) {
		return (NNG_ENOMEM);
	}
	NNI_LIST_INIT(&cq->cq_ents, nni_completion, comp_node);
	if ((rv = nni_mtx_init(&cq->cq_mtx)) != 0) {
		nni_cq_fini(cq);
		NNI_FREE_STRUCT(cq);
	}
	cq->cq_close = 0;
	*cqp = cq;
	return (0);
}


void
nni_cq_fini(nni_cq *cq)
{
	nni_completion *comp;

	nni_mtx_lock(&cq->cq_mtx);
	cq->cq_close = 1;
	while ((comp = nni_list_first(&cq->cq_ents)) != NULL) {
		nni_list_remove(&cq->cq_ents, comp);
		comp->comp_sched = 0;
		comp->comp_result = NNG_ECANCELED;
		nni_taskq_dispatch(nni_main_taskq, &comp->comp_tqe);
	}
	nni_mtx_unlock(&cq->cq_mtx);
	nni_mtx_fini(&cq->cq_mtx);
	NNI_FREE_STRUCT(cq);
}


int
nni_run_cq(nni_cq *cq, int (*func)(nni_completion *, void *), void *arg)
{
	nni_completion *comp;
	nni_completion *ncomp;
	int rv;

	nni_mtx_lock(&cq->cq_mtx);
	ncomp = nni_list_first(&cq->cq_ents);
	while ((comp = ncomp) != NULL) {
		ncomp = nni_list_next(&cq->cq_ents, comp);
		rv = func(comp, arg);
		switch (rv) {
		case 0:
			nni_list_remove(&cq->cq_ents, comp);
			comp->comp_sched = 0;
			nni_taskq_dispatch(nni_main_taskq, &comp->comp_tqe);
			continue;

		case NNG_EAGAIN:
			// Still in progress, and don't stop, just keep
			// checking.
			continue;

		default:
			// All other errors just stop processing.
			nni_mtx_lock(&cq->cq_mtx);
			return (rv);
		}
	}
	nni_mtx_unlock(&cq->cq_mtx);
	return (0);
}


void
nni_cq_cancel(nni_completion *comp)
{
	nni_cq *cq;

	if ((cq = comp->comp_cq) == NULL) {
		return;
	}
	nni_mtx_lock(&cq->cq_mtx);
	if (comp->comp_sched != 0) {
		nni_list_remove(&cq->cq_ents, comp);
		comp->comp_result = NNG_ECANCELED;
		comp->comp_sched = 0;
		nni_taskq_dispatch(nni_main_taskq, &comp->comp_tqe);
	}
	nni_mtx_unlock(&cq->cq_mtx);
}


#if 0
struct nni_cq_expire_arg {
	nni_time	now;
	nni_time	expire;
};

static int
nni_cq_expire(nni_cq *cq, void *arg)
{
	nni_time now = *(nni_time *) arg;

	if (now >= comp->comp_expire) {
		comp->comp_result = NNG_ETIMEDOUT;
		return (0);
	}
	if (comp->comp_expire < cq->cq_expire) {
		cq->cq_expire = comp->comp_expire;
	}
	return (NNG_EAGAIN);
}


#endif

void
nni_cq_submit(nni_completion *comp)
{
	int resched = 0;
	nni_cq *cq = comp->comp_cq;

	nni_mtx_lock(&cq->cq_mtx);
	nni_list_append(&cq->cq_ents, comp);

	if ((comp->comp_expire.t_expire != NNI_TIME_NEVER) &&
	    (comp->comp_expire.t_expire != NNI_TIME_ZERO)) {
		nni_timer_schedule(&comp->comp_expire);
	}
	nni_mtx_unlock(&cq->cq_mtx);

	// XXX schedule expiration
}
