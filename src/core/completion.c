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

// Threads we have waiting...
static nni_thr nni_timer_thr;
static nni_thr *nni_pipe_thrs;
static int nni_pipe_nthrs;
static nni_thr *nni_ep_thrs;
static int nni_ep_nthrs;

static int nni_cp_shutdown;

static nni_mtx nni_timer_mtx;
static nni_cv nni_timer_cv;
static nni_list nni_timer_completions;

static nni_mtx nni_pipe_mtx;
static nni_list nni_pipe_completions;

static nni_mtx nni_ep_mtx;
static nni_list nni_ep_completions;

struct nni_cq {
	nni_list	cq_ents;
	nni_time	cq_expire;
	int		cq_close;
	nni_mtx		cq_mtx;
	nni_cv		cq_cv;
};

static void
nni_cq_timeout(nni_cq *cq)
{
	nni_time now;
	nni_time nni_expire;
	nni_completion *comp;
	nni_completion *ncomp;

	now = nni_clock();
	expire = NNI_TIME_NEVER;

	// We run the entire completion Q, because the queue may not be ordered
	// by time.
	nni_mtx_lock(&cq->cq_mtx);
	ncomp = nni_list_first(&cqe->cq_ents);
	while ((comp = ncomp) != NULL) {
		ncomp = nni_list_next(&cqe->cq_ents, comp);

		if (now >= comp->comp_expire) {
			nni_list_remove(&cqe->cq_ents, comp);
			comp->comp_result = ETIMEDOUT;
			nni_taskq_dispatch(nni_main_taskq, &comp->comp_tqe);
		}
	}
	nni_mtx_unlock(&cq->cq_mtx);
}
