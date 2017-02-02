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
};

int
nni_cq_init(nni_cq **cqp)
{
	nni_cq *cq;
	int rv;

	if ((cq = NNI_ALLOC_STRUCT(cq)) == NULL) {
		return (NNG_ENOMEM);
	}
	NNI_LIST_INIT(&cq->cq_ents, nni_compl, c_node);
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
	nni_compl *c;

	if (cq == NULL) {
		return;
	}
	nni_mtx_lock(&cq->cq_mtx);
	cq->cq_close = 1;
	while ((c = nni_list_first(&cq->cq_ents)) != NULL) {
		nni_list_remove(&cq->cq_ents, c);
		c->c_sched = 0;
		c->c_result = NNG_ECANCELED;
		nni_taskq_dispatch(nni_main_taskq, &c->c_tqe);
	}
	nni_mtx_unlock(&cq->cq_mtx);
	nni_mtx_fini(&cq->cq_mtx);
	NNI_FREE_STRUCT(cq);
}


void
nni_cq_run(nni_cq *cq, int (*func)(nni_compl *, void *), void *arg)
{
	nni_compl *c;
	nni_compl *ncomp;
	int rv;

	nni_mtx_lock(&cq->cq_mtx);
	ncomp = nni_list_first(&cq->cq_ents);
	while ((c = ncomp) != NULL) {
		ncomp = nni_list_next(&cq->cq_ents, c);
		rv = func(c, arg);
		switch (rv) {
		case 0:
			nni_list_remove(&cq->cq_ents, c);
			c->c_sched = 0;
			nni_taskq_dispatch(nni_main_taskq, &c->c_tqe);
			break;
		case NNG_ECONTINUE:
			// Continue running callbacks, but don't remove them.
			nni_taskq_dispatch(nni_main_taskq, &c->c_tqe);
			break;
		default:
			// All others indicate done (should be NNG_EAGAIN).
			nni_mtx_unlock(&cq->cq_mtx);
			return;
		}
	}
	nni_mtx_unlock(&cq->cq_mtx);
}


void
nni_cq_cancel(nni_compl *c)
{
	nni_cq *cq;

	if ((cq = c->c_cq) == NULL) {
		return;
	}
	nni_mtx_lock(&cq->cq_mtx);
	nni_timer_cancel(&c->c_expire);
	if (c->c_sched != 0) {
		nni_list_remove(&cq->cq_ents, c);
		c->c_result = NNG_ECANCELED;
		c->c_sched = 0;
		nni_taskq_dispatch(nni_main_taskq, &c->c_tqe);
	}
	nni_mtx_unlock(&cq->cq_mtx);
}


static void
nni_cq_expire(void *arg)
{
	nni_compl *c = arg;
	nni_cq *cq = c->c_cq;

	nni_mtx_lock(&cq->cq_mtx);
	if (c->c_sched) {
		nni_list_remove(&cq->cq_ents, c);
		c->c_result = NNG_ETIMEDOUT;
		c->c_sched = 0;
		nni_taskq_dispatch(nni_main_taskq, &c->c_tqe);
	}
	nni_mtx_unlock(&cq->cq_mtx);
}


void
nni_compl_submit(nni_compl *c, nni_cq *cq, nni_time expire)
{
	int resched = 0;

	c->c_cq = cq;

	nni_mtx_lock(&cq->cq_mtx);
	nni_list_append(&cq->cq_ents, c);

	c->c_expire.t_expire = expire;
	if ((expire != NNI_TIME_NEVER) && (expire != NNI_TIME_ZERO)) {
		c->c_expire.t_cb = nni_cq_expire;
		c->c_expire.t_arg = c;
		nni_timer_schedule(&c->c_expire);
	}
	nni_mtx_unlock(&cq->cq_mtx);
}


void
nni_compl_init(nni_compl *c, int type, nni_cb cb, void *arg)
{
	memset(c, 0, sizeof (*c));
	NNI_LIST_NODE_INIT(&c->c_node);
	NNI_LIST_NODE_INIT(&c->c_expire.t_node);
	nni_taskq_ent_init(&c->c_tqe, cb, arg);
	c->c_type = type;
}


void
nni_compl_init_canget(nni_compl *c, nni_msgq *mq, nni_cb cb, void *arg)
{
	nni_compl_init(c, NNI_COMPL_TYPE_CANGETMSG, cb, arg);
	c->c_mq = mq;
}


void
nni_compl_init_canput(nni_compl *c, nni_msgq *mq, nni_cb cb, void *arg)
{
	nni_compl_init(c, NNI_COMPL_TYPE_CANPUTMSG, cb, arg);
	c->c_mq = mq;
}
